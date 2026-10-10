package command

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/jsp32io/jsp32/cli/api"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(request *http.Request) (*http.Response, error) {
	return f(request)
}

func jsonResponse(body string) *http.Response {
	return &http.Response{
		StatusCode: http.StatusOK,
		Status:     "200 OK",
		Header:     make(http.Header),
		Body:       io.NopCloser(strings.NewReader(body)),
	}
}

func TestEvalCommandPrintsReturnValue(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		var request api.EvalRequest
		if err := json.NewDecoder(r.Body).Decode(&request); err != nil {
			t.Fatal(err)
		}
		if request.Source != "console.log('1')" {
			t.Errorf("source = %q", request.Source)
		}
		return jsonResponse(`{"ok":true,"async":true,"type":"undefined","value":null,"display":"undefined"}`), nil
	})}

	var stdout bytes.Buffer
	app := &App{
		API:    &api.Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient},
		Stdout: &stdout,
		Stderr: &bytes.Buffer{},
	}
	if err := app.Run(context.Background(), []string{"eval", "console.log('1')"}); err != nil {
		t.Fatal(err)
	}
	if got := stdout.String(); got != "undefined\n" {
		t.Errorf("stdout = %q, want undefined newline", got)
	}
}

func TestEvalCommandReturnsJavaScriptError(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		return jsonResponse(`{"ok":false,"async":true,"error":{"name":"ReferenceError","message":"x is not defined"}}`), nil
	})}

	app := &App{
		API:    &api.Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient},
		Stdout: &bytes.Buffer{},
		Stderr: &bytes.Buffer{},
	}
	err := app.Run(context.Background(), []string{"eval", "x"})
	if err == nil || err.Error() != "ReferenceError: x is not defined" {
		t.Fatalf("error = %v", err)
	}
}

func TestEvalCommandValidatesArguments(t *testing.T) {
	app := &App{Stdout: &bytes.Buffer{}, Stderr: &bytes.Buffer{}}
	err := app.Run(context.Background(), []string{"eval"})
	if _, ok := err.(*UsageError); !ok {
		t.Fatalf("error = %T %v, want *UsageError", err, err)
	}
}
