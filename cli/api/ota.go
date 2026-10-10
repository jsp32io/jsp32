package api

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
)

type OTA struct {
	client *Client
}

func (c *Client) OTA() *OTA {
	return &OTA{client: c}
}

type OTAPartition struct {
	Partition     string `json:"partition"`
	Address       int64  `json:"address"`
	Size          int64  `json:"size"`
	Project       string `json:"project,omitempty"`
	Version       string `json:"version,omitempty"`
	SecureVersion int64  `json:"secureVersion,omitempty"`
	State         string `json:"state,omitempty"`
}

type OTAUploadProgress struct {
	ID             string  `json:"id"`
	Written        *int64  `json:"written,omitempty"`
	WrittenPercent float64 `json:"writtenPercent"`
	Received       int64   `json:"received"`
	Total          int64   `json:"total"`
	Percent        float64 `json:"percent"`
}

type OTARollbackStatus struct {
	Enabled  bool `json:"enabled"`
	Possible bool `json:"possible"`
}

type OTAStatus struct {
	State     string             `json:"state"`
	Running   *OTAPartition      `json:"running"`
	Boot      *OTAPartition      `json:"boot"`
	Next      *OTAPartition      `json:"next"`
	Upload    *OTAUploadProgress `json:"upload"`
	Rollback  OTARollbackStatus  `json:"rollback"`
	LastError *string            `json:"lastError"`
}

type OTAUploadResult struct {
	State    string       `json:"state"`
	SHA256   string       `json:"sha256"`
	Firmware OTAPartition `json:"firmware"`
}

type OTAActionResult struct {
	State   string `json:"state"`
	Changed *bool  `json:"changed,omitempty"`
}

func (ota *OTA) Status(ctx context.Context) (*OTAStatus, error) {
	response, err := ota.do(ctx, http.MethodGet, "ota", nil, -1, nil)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	var result OTAStatus
	if err := decodeJSONResponse(response.Body, &result); err != nil {
		return nil, fmt.Errorf("decode OTA status: %w", err)
	}
	return &result, nil
}

func (ota *OTA) Upload(ctx context.Context, firmware io.Reader, size int64, sha256 string) (*OTAUploadResult, error) {
	return ota.upload(ctx, firmware, size, sha256, "")
}

// UploadWithID correlates status snapshots with this request. ID is 32 lowercase hex digits.
func (ota *OTA) UploadWithID(ctx context.Context, firmware io.Reader, size int64, sha256, id string) (*OTAUploadResult, error) {
	if len(id) != 32 || strings.Trim(id, "0123456789abcdef") != "" {
		return nil, errors.New("upload ID must contain 32 lowercase hex digits")
	}
	return ota.upload(ctx, firmware, size, sha256, id)
}

func (ota *OTA) upload(ctx context.Context, firmware io.Reader, size int64, sha256, id string) (*OTAUploadResult, error) {
	if firmware == nil {
		return nil, errors.New("firmware body is nil")
	}
	if size <= 0 {
		return nil, errors.New("firmware must not be empty")
	}
	if !validSHA256(sha256) {
		return nil, errors.New("firmware checksum must be 64 hexadecimal digits")
	}
	headers := make(http.Header)
	headers.Set("Content-Type", "application/octet-stream")
	headers.Set("X-Content-SHA256", strings.ToLower(sha256))
	if id != "" {
		headers.Set("X-Upload-ID", id)
	}
	response, err := ota.do(ctx, http.MethodPut, "ota/firmware", firmware, size, headers)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	var result OTAUploadResult
	if err := decodeJSONResponse(response.Body, &result); err != nil {
		return nil, fmt.Errorf("decode OTA upload result: %w", err)
	}
	return &result, nil
}

func (ota *OTA) Reboot(ctx context.Context) (*OTAActionResult, error) {
	return ota.action(ctx, "ota/reboot")
}

func (ota *OTA) Confirm(ctx context.Context) (*OTAActionResult, error) {
	return ota.action(ctx, "ota/confirm")
}

func (ota *OTA) Rollback(ctx context.Context) (*OTAActionResult, error) {
	return ota.action(ctx, "ota/rollback")
}

func (ota *OTA) action(ctx context.Context, resource string) (*OTAActionResult, error) {
	response, err := ota.do(ctx, http.MethodPost, resource, http.NoBody, 0, nil)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	var result OTAActionResult
	if err := decodeJSONResponse(response.Body, &result); err != nil {
		return nil, fmt.Errorf("decode OTA action result: %w", err)
	}
	return &result, nil
}

func (ota *OTA) do(ctx context.Context, method, resource string, body io.Reader, size int64, headers http.Header) (*http.Response, error) {
	if ota == nil || ota.client == nil {
		return nil, errors.New("api client is nil")
	}
	endpoint, err := ota.client.endpoint(resource)
	if err != nil {
		return nil, err
	}
	request, err := http.NewRequestWithContext(ctx, method, endpoint, body)
	if err != nil {
		return nil, fmt.Errorf("create OTA request: %w", err)
	}
	if size >= 0 {
		request.ContentLength = size
	}
	for name, values := range headers {
		for _, value := range values {
			request.Header.Add(name, value)
		}
	}
	response, err := ota.client.httpClient().Do(request)
	if err != nil {
		return nil, fmt.Errorf("call JSP32 OTA API: %w", err)
	}
	if response.StatusCode >= http.StatusOK && response.StatusCode < http.StatusMultipleChoices {
		return response, nil
	}
	defer response.Body.Close()
	data, readErr := io.ReadAll(io.LimitReader(response.Body, maxResponseSize+1))
	if readErr != nil {
		return nil, fmt.Errorf("read OTA error response: %w", readErr)
	}
	if len(data) > maxResponseSize {
		return nil, errors.New("OTA error response exceeds 1 MiB")
	}
	var envelope struct {
		Error *EvalError `json:"error"`
	}
	_ = json.Unmarshal(data, &envelope)
	return nil, &HTTPError{StatusCode: response.StatusCode, Status: response.Status, Detail: envelope.Error, Body: strings.TrimSpace(string(data))}
}
