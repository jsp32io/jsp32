package api

import (
	"context"
	"io"
	"net/http"
	"strings"
	"testing"
)

const testSHA256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

func TestFSInfoAndList(t *testing.T) {
	requests := 0
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		requests++
		switch request.URL.Path {
		case "/api/v1/fs/info":
			return jsonResponse(http.StatusOK, `{"ok":true,"filesystem":{"type":"littlefs","totalBytes":1000,"usedBytes":250,"freeBytes":750,"maxNameBytes":253,"directories":true,"atomicReplace":true}}`), nil
		case "/api/v1/fs/files":
			if request.URL.Query().Get("path") != "/settings" ||
				request.URL.Query().Get("limit") != "2" ||
				request.URL.Query().Get("after") != "/a b" {
				t.Errorf("query = %q", request.URL.RawQuery)
			}
			return jsonResponse(http.StatusOK, `{"ok":true,"files":[{"path":"/c","size":3}],"nextAfter":null}`), nil
		default:
			t.Fatalf("unexpected path %q", request.URL.Path)
			return nil, nil
		}
	})}
	fs := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).FS()
	info, err := fs.Info(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if info.Type != "littlefs" || info.FreeBytes != 750 || !info.AtomicReplace {
		t.Errorf("info = %+v", info)
	}
	page, err := fs.List(context.Background(), "/settings", 2, "/a b")
	if err != nil {
		t.Fatal(err)
	}
	if len(page.Files) != 1 || page.Files[0].Path != "/c" || page.NextAfter != nil {
		t.Errorf("page = %+v", page)
	}
	if requests != 2 {
		t.Errorf("requests = %d", requests)
	}
}

func TestFSMkdir(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.Method != http.MethodPost || request.URL.Path != "/api/v1/fs/directory" ||
			request.URL.Query().Get("path") != "/settings" {
			t.Errorf("request = %s %s", request.Method, request.URL)
		}
		return jsonResponse(http.StatusCreated, `{"ok":true,"path":"/settings"}`), nil
	})}
	if err := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).FS().Mkdir(context.Background(), "/settings"); err != nil {
		t.Fatal(err)
	}
}

func TestFSReadAndStatMetadata(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.URL.Path != "/api/v1/fs/file" || request.URL.Query().Get("path") != "/a b.js" {
			t.Errorf("URL = %s", request.URL.String())
		}
		header := make(http.Header)
		header.Set("ETag", `"sha256-`+testSHA256+`"`)
		header.Set("X-File-Size", "3")
		header.Set("Content-Type", "application/javascript")
		body := ""
		if request.Method == http.MethodGet {
			body = "abc"
		}
		return &http.Response{StatusCode: http.StatusOK, Status: "200 OK", Header: header, Body: io.NopCloser(strings.NewReader(body))}, nil
	})}
	fs := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).FS()
	info, err := fs.Stat(context.Background(), "/a b.js")
	if err != nil {
		t.Fatal(err)
	}
	if info.Size != 3 || info.SHA256 != testSHA256 {
		t.Errorf("info = %+v", info)
	}
	download, err := fs.Read(context.Background(), "/a b.js")
	if err != nil {
		t.Fatal(err)
	}
	defer download.Close()
	data, _ := io.ReadAll(download.Body)
	if string(data) != "abc" || download.ContentType != "application/javascript" {
		t.Errorf("download = %+v, body %q", download, data)
	}
}

func TestFSPutSetsIntegrityAndConditionHeaders(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.Method != http.MethodPut || request.ContentLength != 3 {
			t.Errorf("method = %s, length = %d", request.Method, request.ContentLength)
		}
		if got := request.Header.Get("If-Match"); got != `"sha256-`+testSHA256+`"` {
			t.Errorf("If-Match = %q", got)
		}
		if got := request.Header.Get("X-Content-SHA256"); got != testSHA256 {
			t.Errorf("X-Content-SHA256 = %q", got)
		}
		data, _ := io.ReadAll(request.Body)
		if string(data) != "abc" {
			t.Errorf("body = %q", data)
		}
		return jsonResponse(http.StatusOK, `{"ok":true,"file":{"path":"/x","size":3,"sha256":"`+testSHA256+`","created":false,"restartRequired":false}}`), nil
	})}
	fs := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).FS()
	result, err := fs.Put(context.Background(), "/x", strings.NewReader("abc"), 3, PutOptions{
		MatchSHA256: testSHA256, ContentSHA256: testSHA256,
	})
	if err != nil {
		t.Fatal(err)
	}
	if result.Path != "/x" || result.Created {
		t.Errorf("result = %+v", result)
	}
}

func TestFSPutEmptyFileUsesKnownZeroLength(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.ContentLength != 0 || request.Body != http.NoBody {
			t.Errorf("length = %d, body = %#v", request.ContentLength, request.Body)
		}
		return jsonResponse(http.StatusCreated, `{"ok":true,"file":{"path":"/empty","size":0,"sha256":"","created":true,"restartRequired":false}}`), nil
	})}
	fs := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).FS()
	if _, err := fs.Put(context.Background(), "/empty", strings.NewReader(""), 0, PutOptions{}); err != nil {
		t.Fatal(err)
	}
}

func TestFSDeleteAndStructuredError(t *testing.T) {
	calls := 0
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		calls++
		if calls == 1 {
			if request.Method != http.MethodDelete || request.Header.Get("If-Match") == "" {
				t.Errorf("delete request = %+v", request)
			}
			return jsonResponse(http.StatusNoContent, ""), nil
		}
		return jsonResponse(http.StatusNotFound, `{"ok":false,"error":{"code":"FS_NOT_FOUND","name":"NotFoundError","message":"file not found"}}`), nil
	})}
	fs := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).FS()
	if err := fs.Delete(context.Background(), "/x", testSHA256); err != nil {
		t.Fatal(err)
	}
	_, err := fs.Stat(context.Background(), "/missing")
	httpError, ok := err.(*HTTPError)
	if !ok || httpError.Detail == nil || httpError.Detail.Code != "FS_NOT_FOUND" || err.Error() != "NotFoundError: file not found" {
		t.Fatalf("error = %#v", err)
	}
}

func TestFSListRejectsInvalidLimit(t *testing.T) {
	_, err := (&Client{}).FS().List(context.Background(), "/", 101, "")
	if err == nil {
		t.Fatal("expected invalid limit error")
	}
}
