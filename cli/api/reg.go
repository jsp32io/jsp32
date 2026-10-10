package api

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
)

type Reg struct{ client *Client }

func (c *Client) Reg() *Reg { return &Reg{client: c} }

type RegInfo struct {
	Revision       string         `json:"revision"`
	Areas          map[string]int `json:"areas"`
	MaxTransaction int            `json:"maxTransaction"`
	Volatile       bool           `json:"volatile"`
}

type RegValue struct {
	Address string          `json:"address"`
	Value   json.RawMessage `json:"value"`
}

type RegSnapshot struct {
	Revision string     `json:"revision"`
	Values   []RegValue `json:"values"`
}

type RegWrite struct {
	Address string `json:"address"`
	Value   any    `json:"value"`
}

type RegWriteResult struct {
	Revision string `json:"revision"`
}

type RegChange struct {
	Revision string          `json:"r"`
	Address  string          `json:"a"`
	Value    json.RawMessage `json:"v"`
}

func (r *Reg) Info(ctx context.Context) (*RegInfo, error) {
	var result RegInfo
	if err := r.call(ctx, http.MethodGet, "registers", nil, &result); err != nil {
		return nil, err
	}
	return &result, nil
}

func (r *Reg) Read(ctx context.Context, addresses []string) (*RegSnapshot, error) {
	var result RegSnapshot
	if err := r.call(ctx, http.MethodPost, "registers/read", map[string]any{"addresses": addresses}, &result); err != nil {
		return nil, err
	}
	return &result, nil
}

func (r *Reg) ReadRange(ctx context.Context, start string, count int) (*RegSnapshot, error) {
	var result RegSnapshot
	if err := r.call(ctx, http.MethodPost, "registers/read", map[string]any{"start": start, "count": count}, &result); err != nil {
		return nil, err
	}
	return &result, nil
}

func (r *Reg) Write(ctx context.Context, writes []RegWrite) (*RegWriteResult, error) {
	var result RegWriteResult
	if err := r.call(ctx, http.MethodPost, "registers/write", map[string]any{"writes": writes}, &result); err != nil {
		return nil, err
	}
	return &result, nil
}

func (r *Reg) WriteRange(ctx context.Context, start string, values []any) (*RegWriteResult, error) {
	var result RegWriteResult
	if err := r.call(ctx, http.MethodPost, "registers/write", map[string]any{"start": start, "values": values}, &result); err != nil {
		return nil, err
	}
	return &result, nil
}

func (r *Reg) call(ctx context.Context, method, path string, input, output any) error {
	if r == nil || r.client == nil {
		return errors.New("api client is nil")
	}
	endpoint, err := r.client.endpoint(path)
	if err != nil {
		return err
	}
	var body io.Reader
	if input != nil {
		data, err := json.Marshal(input)
		if err != nil {
			return err
		}
		body = bytes.NewReader(data)
	}
	request, err := http.NewRequestWithContext(ctx, method, endpoint, body)
	if err != nil {
		return fmt.Errorf("create register request: %w", err)
	}
	if input != nil {
		request.Header.Set("Content-Type", "application/json")
	}
	response, err := r.client.httpClient().Do(request)
	if err != nil {
		return fmt.Errorf("call JSP32 register API: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		data, _ := io.ReadAll(io.LimitReader(response.Body, maxResponseSize))
		var envelope struct {
			Error *EvalError `json:"error"`
		}
		_ = json.Unmarshal(data, &envelope)
		return &HTTPError{StatusCode: response.StatusCode, Status: response.Status, Detail: envelope.Error, Body: strings.TrimSpace(string(data))}
	}
	return decodeJSONResponse(response.Body, output)
}

// Watch returns a stream of native register changes. Close it when finished.
func (r *Reg) Watch(ctx context.Context, addresses ...string) (*ConsoleStream, error) {
	if r == nil || r.client == nil {
		return nil, errors.New("api client is nil")
	}
	endpoint, err := r.client.endpoint("registers/events")
	if err != nil {
		return nil, err
	}
	if len(addresses) > 0 {
		endpoint += "?" + url.Values{"addresses": {strings.Join(addresses, ",")}}.Encode()
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, err
	}
	request.Header.Set("Accept", "text/event-stream")
	response, err := r.client.httpClient().Do(request)
	if err != nil {
		return nil, err
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		defer response.Body.Close()
		data, _ := io.ReadAll(io.LimitReader(response.Body, maxResponseSize))
		return nil, &HTTPError{StatusCode: response.StatusCode, Status: response.Status, Body: strings.TrimSpace(string(data))}
	}
	return newConsoleStream(response.Body), nil
}
