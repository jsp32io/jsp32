package api

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) {
	return f(request)
}

func jsonResponse(statusCode int, body string) *http.Response {
	return &http.Response{
		StatusCode: statusCode,
		Status:     http.StatusText(statusCode),
		Header:     make(http.Header),
		Body:       io.NopCloser(strings.NewReader(body)),
	}
}

func TestJSEvalAddsDefaultAPIVersion(t *testing.T) {
	var received EvalRequest
	httpClient := &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path != "/api/v1/quickjs/eval" {
			t.Errorf("path = %q, want /api/v1/quickjs/eval", r.URL.Path)
		}
		if r.Method != http.MethodPost {
			t.Errorf("method = %q, want POST", r.Method)
		}
		if got := r.Header.Get("Content-Type"); got != "application/json" {
			t.Errorf("Content-Type = %q, want application/json", got)
		}
		if err := json.NewDecoder(r.Body).Decode(&received); err != nil {
			t.Fatal(err)
		}
		return jsonResponse(http.StatusOK, `{"ok":true,"async":true,"type":"number","value":2,"display":"2"}`), nil
	})}

	client := &Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}
	result, err := client.JS().Eval(context.Background(), &EvalRequest{
		Source: "1 + 1", Mode: EvalModeExpr, Await: true, TimeoutMS: 5000,
	})
	if err != nil {
		t.Fatal(err)
	}
	if result.Display != "2" {
		t.Errorf("display = %q, want 2", result.Display)
	}
	if received.Source != "1 + 1" || received.Mode != EvalModeExpr || !received.Await || received.TimeoutMS != 5000 {
		t.Errorf("unexpected request: %+v", received)
	}
}

func TestJSEvalKeepsExplicitAPIVersion(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path != "/api/v2/quickjs/eval" {
			t.Errorf("path = %q, want /api/v2/quickjs/eval", r.URL.Path)
		}
		return jsonResponse(http.StatusOK, `{"ok":true,"type":"undefined","value":null,"display":"undefined"}`), nil
	})}

	client := &Client{BaseURL: "http://jsp32.local/api/v2/", HTTPClient: httpClient}
	if _, err := client.JS().Eval(context.Background(), &EvalRequest{Source: "void 0"}); err != nil {
		t.Fatal(err)
	}
}

func TestJSEvalRequiresBaseURL(t *testing.T) {
	client := &Client{}
	_, err := client.JS().Eval(context.Background(), &EvalRequest{Source: "1"})
	if err == nil || err.Error() != "JSP32_BASE_URL is required" {
		t.Fatalf("error = %v, want JSP32_BASE_URL is required", err)
	}
}

func TestJSEvalReturnsStructuredHTTPError(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		return jsonResponse(http.StatusServiceUnavailable, `{"ok":false,"error":{"name":"RuntimeUnavailableError","message":"runtime stopped"}}`), nil
	})}

	client := &Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}
	_, err := client.JS().Eval(context.Background(), &EvalRequest{Source: "1"})
	httpError, ok := err.(*HTTPError)
	if !ok {
		t.Fatalf("error = %T %v, want *HTTPError", err, err)
	}
	if httpError.StatusCode != http.StatusServiceUnavailable || httpError.Error() != "RuntimeUnavailableError: runtime stopped" {
		t.Errorf("unexpected HTTP error: %+v", httpError)
	}
}

func TestJSEvalReturnsPlainHTTPErrorBeforeDecodingJSON(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		return &http.Response{
			StatusCode: http.StatusNotFound,
			Status:     "404 Not Found",
			Header:     make(http.Header),
			Body:       io.NopCloser(strings.NewReader("Not found")),
		}, nil
	})}

	client := &Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}
	_, err := client.JS().Eval(context.Background(), &EvalRequest{Source: "1"})
	httpError, ok := err.(*HTTPError)
	if !ok {
		t.Fatalf("error = %T %v, want *HTTPError", err, err)
	}
	if got := httpError.Error(); got != "JSP32 API returned 404 Not Found: Not found" {
		t.Errorf("error = %q", got)
	}
}
