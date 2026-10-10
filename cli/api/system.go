package api

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
)

type System struct {
	client *Client
}

func (c *Client) System() *System {
	return &System{client: c}
}

type SystemInfo struct {
	UptimeMs int64 `json:"uptimeMs"`
	// Raw preserves the complete response, including fields added by newer firmware.
	Raw json.RawMessage `json:"-"`
}

func (system *System) Info(ctx context.Context) (*SystemInfo, error) {
	if system == nil || system.client == nil {
		return nil, errors.New("api client is nil")
	}
	endpoint, err := system.client.endpoint("system/info")
	if err != nil {
		return nil, err
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, fmt.Errorf("create system info request: %w", err)
	}
	response, err := system.client.httpClient().Do(request)
	if err != nil {
		return nil, fmt.Errorf("query system info: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("query system info: %s", response.Status)
	}
	var raw json.RawMessage
	if err := json.NewDecoder(response.Body).Decode(&raw); err != nil {
		return nil, fmt.Errorf("decode system info: %w", err)
	}
	var payload struct {
		UptimeMs *int64 `json:"uptimeMs"`
	}
	if err := json.Unmarshal(raw, &payload); err != nil {
		return nil, fmt.Errorf("decode system info: %w", err)
	}
	if payload.UptimeMs == nil || *payload.UptimeMs < 0 {
		return nil, fmt.Errorf("system info has no valid uptimeMs")
	}
	return &SystemInfo{UptimeMs: *payload.UptimeMs, Raw: raw}, nil
}
