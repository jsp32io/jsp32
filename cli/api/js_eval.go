package api

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
)

const maxResponseSize = 1024 * 1024

type EvalMode string

const (
	EvalModeExpr   EvalMode = "expr"
	EvalModeScript EvalMode = "script"
)

type EvalRequest struct {
	Source    string   `json:"source"`
	Mode      EvalMode `json:"mode"`
	Await     bool     `json:"await"`
	TimeoutMS int64    `json:"timeoutMs"`
}

type EvalError struct {
	Code    string `json:"code,omitempty"`
	Name    string `json:"name"`
	Message string `json:"message"`
	Stack   string `json:"stack,omitempty"`
}

func (e *EvalError) Error() string {
	if e == nil {
		return "JavaScript evaluation failed"
	}
	if e.Name == "" {
		return e.Message
	}
	if e.Message == "" {
		return e.Name
	}
	return e.Name + ": " + e.Message
}

type EvalResult struct {
	OK        bool            `json:"ok"`
	Async     bool            `json:"async"`
	Type      string          `json:"type,omitempty"`
	Value     json.RawMessage `json:"value,omitempty"`
	Display   string          `json:"display,omitempty"`
	Truncated bool            `json:"truncated,omitempty"`
	Error     *EvalError      `json:"error,omitempty"`
}

type HTTPError struct {
	StatusCode int
	Status     string
	Detail     *EvalError
	Body       string
}

func (e *HTTPError) Error() string {
	if e.Detail != nil {
		return e.Detail.Error()
	}
	if e.Body != "" {
		return "JSP32 API returned " + e.Status + ": " + e.Body
	}
	return "JSP32 API returned " + e.Status
}

func (js *JS) Eval(ctx context.Context, evalRequest *EvalRequest) (*EvalResult, error) {
	if js == nil || js.client == nil {
		return nil, errors.New("api client is nil")
	}
	if evalRequest == nil {
		return nil, errors.New("eval request is nil")
	}

	endpoint, err := js.client.endpoint("quickjs/eval")
	if err != nil {
		return nil, err
	}
	body, err := json.Marshal(evalRequest)
	if err != nil {
		return nil, fmt.Errorf("encode eval request: %w", err)
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, bytes.NewReader(body))
	if err != nil {
		return nil, fmt.Errorf("create eval request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Accept", "application/json")

	response, err := js.client.httpClient().Do(req)
	if err != nil {
		return nil, fmt.Errorf("call JSP32 API: %w", err)
	}
	defer response.Body.Close()

	responseBody, err := io.ReadAll(io.LimitReader(response.Body, maxResponseSize+1))
	if err != nil {
		return nil, fmt.Errorf("read eval response: %w", err)
	}
	if len(responseBody) > maxResponseSize {
		return nil, errors.New("eval response exceeds 1 MiB")
	}

	if response.StatusCode < http.StatusOK || response.StatusCode >= http.StatusMultipleChoices {
		var errorResponse struct {
			Error *EvalError `json:"error"`
		}
		_ = json.Unmarshal(responseBody, &errorResponse)
		return nil, &HTTPError{
			StatusCode: response.StatusCode,
			Status:     response.Status,
			Detail:     errorResponse.Error,
			Body:       strings.TrimSpace(string(responseBody)),
		}
	}

	var result EvalResult
	if err := json.Unmarshal(responseBody, &result); err != nil {
		return nil, fmt.Errorf("decode eval response: %w", err)
	}
	return &result, nil
}
