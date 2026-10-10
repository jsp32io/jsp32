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
	"strconv"
	"strings"
)

type NVS struct {
	client *Client
}

func (c *Client) NVS() *NVS {
	return &NVS{client: c}
}

type NVSInfo struct {
	Partition      string    `json:"partition"`
	Size           int64     `json:"size"`
	Encrypted      bool      `json:"encrypted"`
	UsedEntries    int64     `json:"usedEntries"`
	FreeEntries    int64     `json:"freeEntries"`
	TotalEntries   int64     `json:"totalEntries"`
	NamespaceCount int64     `json:"namespaceCount"`
	Limits         NVSLimits `json:"limits"`
}

type NVSLimits struct {
	MaxStringBytes     int64 `json:"maxStringBytes"`
	MaxBlobBytes       int64 `json:"maxBlobBytes"`
	MaxBatchOperations int64 `json:"maxBatchOperations"`
}

type NVSNamespace struct {
	Name        string `json:"name"`
	Policy      string `json:"policy"`
	UsedEntries int64  `json:"usedEntries"`
	Writable    bool   `json:"writable"`
}

type NVSNamespacePage struct {
	Namespaces []NVSNamespace `json:"namespaces"`
	HasMore    bool           `json:"hasMore"`
	NextAfter  *string        `json:"nextAfter"`
}

type NVSEntry struct {
	Key      string `json:"key"`
	Type     string `json:"type"`
	Size     *int64 `json:"size"`
	Readable bool   `json:"readable"`
	Writable bool   `json:"writable"`
}

type NVSEntryPage struct {
	Namespace string     `json:"namespace"`
	Entries   []NVSEntry `json:"entries"`
	HasMore   bool       `json:"hasMore"`
	NextAfter *string    `json:"nextAfter"`
}

type NVSValue struct {
	Namespace string          `json:"namespace"`
	Key       string          `json:"key"`
	Type      string          `json:"type"`
	Encoding  string          `json:"encoding,omitempty"`
	Value     json.RawMessage `json:"value"`
	ETag      string          `json:"etag"`
}

type NVSWriteOptions struct {
	CreateOnly  bool
	MatchSHA256 string
}

type NVSBatchResult struct {
	Namespace  string          `json:"namespace"`
	Operations json.RawMessage `json:"operations"`
}

func (nvs *NVS) Info(ctx context.Context) (*NVSInfo, error) {
	var data NVSInfo
	if err := nvs.json(ctx, http.MethodGet, "nvs", nil, nil, &data); err != nil {
		return nil, err
	}
	return &data, nil
}

func (nvs *NVS) ListNamespaces(ctx context.Context, prefix string, limit int, after string) (*NVSNamespacePage, error) {
	if err := validateNVSPageLimit(limit); err != nil {
		return nil, err
	}
	query := paginationQuery(prefix, limit, after)
	var data NVSNamespacePage
	if err := nvs.json(ctx, http.MethodGet, "nvs/namespaces?"+query.Encode(), nil, nil, &data); err != nil {
		return nil, err
	}
	return &data, nil
}

func (nvs *NVS) Namespace(ctx context.Context, namespace string) (*NVSNamespace, error) {
	var data NVSNamespace
	if err := nvs.json(ctx, http.MethodGet, namespaceResource(namespace), nil, nil, &data); err != nil {
		return nil, err
	}
	return &data, nil
}

func (nvs *NVS) ListEntries(ctx context.Context, namespace, prefix string, limit int, after string) (*NVSEntryPage, error) {
	if err := validateNVSPageLimit(limit); err != nil {
		return nil, err
	}
	query := paginationQuery(prefix, limit, after)
	var data NVSEntryPage
	resource := namespaceResource(namespace) + "/entries?" + query.Encode()
	if err := nvs.json(ctx, http.MethodGet, resource, nil, nil, &data); err != nil {
		return nil, err
	}
	return &data, nil
}

func (nvs *NVS) Get(ctx context.Context, namespace, key string) (*NVSValue, error) {
	var data NVSValue
	if err := nvs.json(ctx, http.MethodGet, entryResource(namespace, key), nil, nil, &data); err != nil {
		return nil, err
	}
	return &data, nil
}

func (nvs *NVS) Set(ctx context.Context, namespace, key string, value any, options NVSWriteOptions) (*NVSValue, error) {
	if options.CreateOnly && options.MatchSHA256 != "" {
		return nil, errors.New("create-only and match checksum are mutually exclusive")
	}
	headers := make(http.Header)
	if options.CreateOnly {
		headers.Set("If-None-Match", "*")
	}
	if options.MatchSHA256 != "" {
		if !validSHA256(options.MatchSHA256) {
			return nil, errors.New("match checksum must be 64 hexadecimal digits")
		}
		headers.Set("If-Match", makeETag(options.MatchSHA256))
	}
	var data NVSValue
	if err := nvs.json(ctx, http.MethodPut, entryResource(namespace, key), value, headers, &data); err != nil {
		return nil, err
	}
	return &data, nil
}

func (nvs *NVS) Delete(ctx context.Context, namespace, key, matchSHA256 string) error {
	headers := make(http.Header)
	if matchSHA256 != "" {
		if !validSHA256(matchSHA256) {
			return errors.New("match checksum must be 64 hexadecimal digits")
		}
		headers.Set("If-Match", makeETag(matchSHA256))
	}
	response, err := nvs.do(ctx, http.MethodDelete, entryResource(namespace, key), nil, headers)
	if err != nil {
		return err
	}
	return response.Body.Close()
}

func (nvs *NVS) Clear(ctx context.Context, namespace string) error {
	headers := make(http.Header)
	headers.Set("X-Confirm-Namespace", namespace)
	response, err := nvs.do(ctx, http.MethodDelete, namespaceResource(namespace), nil, headers)
	if err != nil {
		return err
	}
	return response.Body.Close()
}

func (nvs *NVS) Batch(ctx context.Context, namespace string, body json.RawMessage) (*NVSBatchResult, error) {
	if !json.Valid(body) {
		return nil, errors.New("batch body is not valid JSON")
	}
	var data NVSBatchResult
	if err := nvs.json(ctx, http.MethodPost, namespaceResource(namespace)+"/batch", body, nil, &data); err != nil {
		return nil, err
	}
	return &data, nil
}

func (nvs *NVS) json(ctx context.Context, method, resource string, input any, headers http.Header, output any) error {
	var body io.Reader
	if input != nil {
		encoded, err := json.Marshal(input)
		if err != nil {
			return fmt.Errorf("encode NVS request: %w", err)
		}
		body = bytes.NewReader(encoded)
		if headers == nil {
			headers = make(http.Header)
		}
		headers.Set("Content-Type", "application/json")
	}
	response, err := nvs.do(ctx, method, resource, body, headers)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	var envelope struct {
		Data json.RawMessage `json:"data"`
	}
	if err := decodeJSONResponse(response.Body, &envelope); err != nil {
		return fmt.Errorf("decode NVS response: %w", err)
	}
	if err := json.Unmarshal(envelope.Data, output); err != nil {
		return fmt.Errorf("decode NVS response data: %w", err)
	}
	return nil
}

func (nvs *NVS) do(ctx context.Context, method, resource string, body io.Reader, headers http.Header) (*http.Response, error) {
	if nvs == nil || nvs.client == nil {
		return nil, errors.New("api client is nil")
	}
	resourcePath, rawQuery, _ := strings.Cut(resource, "?")
	endpoint, err := nvs.client.endpoint(resourcePath)
	if err != nil {
		return nil, err
	}
	if rawQuery != "" {
		endpoint += "?" + rawQuery
	}
	request, err := http.NewRequestWithContext(ctx, method, endpoint, body)
	if err != nil {
		return nil, fmt.Errorf("create NVS request: %w", err)
	}
	for name, values := range headers {
		for _, value := range values {
			request.Header.Add(name, value)
		}
	}
	response, err := nvs.client.httpClient().Do(request)
	if err != nil {
		return nil, fmt.Errorf("call JSP32 NVS API: %w", err)
	}
	if response.StatusCode >= http.StatusOK && response.StatusCode < http.StatusMultipleChoices {
		return response, nil
	}
	defer response.Body.Close()
	data, readErr := io.ReadAll(io.LimitReader(response.Body, maxResponseSize+1))
	if readErr != nil {
		return nil, fmt.Errorf("read NVS error response: %w", readErr)
	}
	if len(data) > maxResponseSize {
		return nil, errors.New("NVS error response exceeds 1 MiB")
	}
	var envelope struct {
		Error *EvalError `json:"error"`
	}
	_ = json.Unmarshal(data, &envelope)
	return nil, &HTTPError{StatusCode: response.StatusCode, Status: response.Status, Detail: envelope.Error, Body: strings.TrimSpace(string(data))}
}

func validateNVSPageLimit(limit int) error {
	if limit < 1 || limit > 100 {
		return errors.New("NVS list limit must be from 1 to 100")
	}
	return nil
}

func paginationQuery(prefix string, limit int, after string) url.Values {
	query := url.Values{"limit": {strconv.Itoa(limit)}}
	if prefix != "" {
		query.Set("prefix", prefix)
	}
	if after != "" {
		query.Set("after", after)
	}
	return query
}

func namespaceResource(namespace string) string {
	return "nvs/namespaces/" + url.PathEscape(namespace)
}

func entryResource(namespace, key string) string {
	return namespaceResource(namespace) + "/entries/" + url.PathEscape(key)
}
