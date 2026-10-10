package api

import (
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

// FS provides access to the device filesystem API.
type FS struct {
	client *Client
}

// FS returns the filesystem API group for this client.
func (c *Client) FS() *FS {
	return &FS{client: c}
}

type FilesystemInfo struct {
	Type          string `json:"type"`
	TotalBytes    int64  `json:"totalBytes"`
	UsedBytes     int64  `json:"usedBytes"`
	FreeBytes     int64  `json:"freeBytes"`
	MaxNameBytes  int64  `json:"maxNameBytes"`
	Directories   bool   `json:"directories"`
	AtomicReplace bool   `json:"atomicReplace"`
}

type FileInfo struct {
	Path      string `json:"path"`
	Size      int64  `json:"size"`
	SHA256    string `json:"sha256,omitempty"`
	Directory bool   `json:"directory,omitempty"`
}

type FilePage struct {
	Files     []FileInfo `json:"files"`
	NextAfter *string    `json:"nextAfter"`
}

type FileDownload struct {
	FileInfo
	ContentType string
	Body        io.ReadCloser
}

func (f *FileDownload) Close() error {
	if f == nil || f.Body == nil {
		return nil
	}
	return f.Body.Close()
}

type PutOptions struct {
	CreateOnly    bool
	MatchSHA256   string
	ContentSHA256 string
	ContentType   string
}

type PutResult struct {
	Path            string `json:"path"`
	Size            int64  `json:"size"`
	SHA256          string `json:"sha256"`
	Created         bool   `json:"created"`
	RestartRequired bool   `json:"restartRequired"`
}

func (fs *FS) Info(ctx context.Context) (*FilesystemInfo, error) {
	response, err := fs.do(ctx, http.MethodGet, "fs/info", nil, -1, nil)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	var envelope struct {
		Filesystem FilesystemInfo `json:"filesystem"`
	}
	if err := decodeJSONResponse(response.Body, &envelope); err != nil {
		return nil, fmt.Errorf("decode filesystem info: %w", err)
	}
	return &envelope.Filesystem, nil
}

func (fs *FS) List(ctx context.Context, directory string, limit int, after string) (*FilePage, error) {
	if limit < 1 || limit > 100 {
		return nil, errors.New("filesystem list limit must be from 1 to 100")
	}
	query := url.Values{"limit": {strconv.Itoa(limit)}}
	if directory != "" && directory != "/" {
		query.Set("path", directory)
	}
	if after != "" {
		query.Set("after", after)
	}
	response, err := fs.do(ctx, http.MethodGet, "fs/files?"+query.Encode(), nil, -1, nil)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	var envelope struct {
		Files     []FileInfo `json:"files"`
		NextAfter *string    `json:"nextAfter"`
	}
	if err := decodeJSONResponse(response.Body, &envelope); err != nil {
		return nil, fmt.Errorf("decode filesystem file list: %w", err)
	}
	return &FilePage{Files: envelope.Files, NextAfter: envelope.NextAfter}, nil
}

func (fs *FS) Mkdir(ctx context.Context, path string) error {
	response, err := fs.do(ctx, http.MethodPost, "fs/directory?"+
		url.Values{"path": {path}}.Encode(), http.NoBody, 0, nil)
	if err != nil {
		return err
	}
	return response.Body.Close()
}

func (fs *FS) Stat(ctx context.Context, path string) (*FileInfo, error) {
	response, err := fs.do(ctx, http.MethodHead, fileResource(path), nil, -1, nil)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	return fileInfoFromHeaders(path, response.Header)
}

func (fs *FS) Read(ctx context.Context, path string) (*FileDownload, error) {
	response, err := fs.do(ctx, http.MethodGet, fileResource(path), nil, -1, nil)
	if err != nil {
		return nil, err
	}
	info, err := fileInfoFromHeaders(path, response.Header)
	if err != nil {
		response.Body.Close()
		return nil, err
	}
	return &FileDownload{
		FileInfo:    *info,
		ContentType: response.Header.Get("Content-Type"),
		Body:        response.Body,
	}, nil
}

func (fs *FS) Put(ctx context.Context, path string, body io.Reader, size int64, options PutOptions) (*PutResult, error) {
	if body == nil {
		return nil, errors.New("file body is nil")
	}
	if size < 0 {
		return nil, errors.New("file size must not be negative")
	}
	if options.CreateOnly && options.MatchSHA256 != "" {
		return nil, errors.New("create-only and match checksum are mutually exclusive")
	}
	if options.MatchSHA256 != "" && !validSHA256(options.MatchSHA256) {
		return nil, errors.New("match checksum must be 64 hexadecimal digits")
	}
	if options.ContentSHA256 != "" && !validSHA256(options.ContentSHA256) {
		return nil, errors.New("content checksum must be 64 hexadecimal digits")
	}
	if size == 0 {
		body = http.NoBody
	}
	headers := make(http.Header)
	if options.ContentType == "" {
		headers.Set("Content-Type", "application/octet-stream")
	} else {
		headers.Set("Content-Type", options.ContentType)
	}
	if options.CreateOnly {
		headers.Set("If-None-Match", "*")
	}
	if options.MatchSHA256 != "" {
		headers.Set("If-Match", makeETag(options.MatchSHA256))
	}
	if options.ContentSHA256 != "" {
		headers.Set("X-Content-SHA256", options.ContentSHA256)
	}
	response, err := fs.do(ctx, http.MethodPut, fileResource(path), body, size, headers)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	var envelope struct {
		File PutResult `json:"file"`
	}
	if err := decodeJSONResponse(response.Body, &envelope); err != nil {
		return nil, fmt.Errorf("decode filesystem upload result: %w", err)
	}
	return &envelope.File, nil
}

func (fs *FS) Delete(ctx context.Context, path, matchSHA256 string) error {
	if matchSHA256 != "" && !validSHA256(matchSHA256) {
		return errors.New("match checksum must be 64 hexadecimal digits")
	}
	headers := make(http.Header)
	if matchSHA256 != "" {
		headers.Set("If-Match", makeETag(matchSHA256))
	}
	response, err := fs.do(ctx, http.MethodDelete, fileResource(path), nil, -1, headers)
	if err != nil {
		return err
	}
	return response.Body.Close()
}

func (fs *FS) do(ctx context.Context, method, resource string, body io.Reader, size int64, headers http.Header) (*http.Response, error) {
	if fs == nil || fs.client == nil {
		return nil, errors.New("api client is nil")
	}
	resourcePath, rawQuery, _ := strings.Cut(resource, "?")
	endpoint, err := fs.client.endpoint(resourcePath)
	if err != nil {
		return nil, err
	}
	if rawQuery != "" {
		endpoint += "?" + rawQuery
	}
	request, err := http.NewRequestWithContext(ctx, method, endpoint, body)
	if err != nil {
		return nil, fmt.Errorf("create filesystem request: %w", err)
	}
	if size >= 0 {
		request.ContentLength = size
	}
	for name, values := range headers {
		for _, value := range values {
			request.Header.Add(name, value)
		}
	}
	response, err := fs.client.httpClient().Do(request)
	if err != nil {
		return nil, fmt.Errorf("call JSP32 filesystem API: %w", err)
	}
	if response.StatusCode >= http.StatusOK && response.StatusCode < http.StatusMultipleChoices {
		return response, nil
	}
	defer response.Body.Close()
	data, readErr := io.ReadAll(io.LimitReader(response.Body, maxResponseSize+1))
	if readErr != nil {
		return nil, fmt.Errorf("read filesystem error response: %w", readErr)
	}
	if len(data) > maxResponseSize {
		return nil, errors.New("filesystem error response exceeds 1 MiB")
	}
	var envelope struct {
		Error *EvalError `json:"error"`
	}
	_ = json.Unmarshal(data, &envelope)
	return nil, &HTTPError{
		StatusCode: response.StatusCode,
		Status:     response.Status,
		Detail:     envelope.Error,
		Body:       strings.TrimSpace(string(data)),
	}
}

func decodeJSONResponse(reader io.Reader, target any) error {
	data, err := io.ReadAll(io.LimitReader(reader, maxResponseSize+1))
	if err != nil {
		return err
	}
	if len(data) > maxResponseSize {
		return errors.New("response exceeds 1 MiB")
	}
	return json.Unmarshal(data, target)
}

func fileResource(path string) string {
	return "fs/file?" + url.Values{"path": {path}}.Encode()
}

func fileInfoFromHeaders(path string, headers http.Header) (*FileInfo, error) {
	size, err := strconv.ParseInt(headers.Get("X-File-Size"), 10, 64)
	if err != nil || size < 0 {
		return nil, errors.New("filesystem response has invalid X-File-Size")
	}
	sha256, err := parseETag(headers.Get("ETag"))
	if err != nil {
		return nil, err
	}
	return &FileInfo{Path: path, Size: size, SHA256: sha256}, nil
}

func parseETag(value string) (string, error) {
	if len(value) != len(`"sha256-"`)+64 || !strings.HasPrefix(value, `"sha256-`) || !strings.HasSuffix(value, `"`) {
		return "", errors.New("filesystem response has invalid ETag")
	}
	value = strings.TrimSuffix(strings.TrimPrefix(value, `"sha256-`), `"`)
	if !validSHA256(value) {
		return "", errors.New("filesystem response has invalid ETag")
	}
	return strings.ToLower(value), nil
}

func validSHA256(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, character := range value {
		if !strings.ContainsRune("0123456789abcdefABCDEF", character) {
			return false
		}
	}
	return true
}

func makeETag(sha256 string) string {
	return `"sha256-` + sha256 + `"`
}
