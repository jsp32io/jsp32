package command

import (
	"bytes"
	"context"
	"errors"
	"github.com/jsp32io/jsp32/cli/api"
	"net/http"
	"strings"
	"testing"
)

func TestManCommand(t *testing.T) {
	for _, tc := range []struct {
		args  []string
		topic string
	}{
		{[]string{"man"}, "index"}, {[]string{"man", "js.gpio"}, "js.gpio"}, {[]string{"man", "js"}, "js"},
	} {
		out := &bytes.Buffer{}
		const markdown = "# 手册\n\n原始 Markdown，无额外换行"
		app := &App{Stdout: out, API: &api.Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
			if r.URL.Path != "/api/v1/man/"+tc.topic {
				t.Errorf("path: %s", r.URL.Path)
			}
			return jsonResponse(markdown), nil
		})}}}
		if err := app.Run(context.Background(), tc.args); err != nil {
			t.Fatal(err)
		}
		if out.String() != markdown {
			t.Fatalf("output: %q", out.String())
		}
	}
}

func TestManHelpAndInvalidArguments(t *testing.T) {
	out := &bytes.Buffer{}
	app := &App{Stdout: out}
	for _, args := range [][]string{{"help"}, {"man", "--help"}, {"man", "help"}} {
		out.Reset()
		if err := app.Run(context.Background(), args); err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(out.String(), "man") {
			t.Errorf("missing help: %q", out.String())
		}
	}
	for _, args := range [][]string{{"man", "a", "b"}, {"man", "../secret"}, {"man", "--unknown"}, {"man", ""}} {
		var usage *UsageError
		if err := app.Run(context.Background(), args); !errors.As(err, &usage) {
			t.Errorf("args %v: %v", args, err)
		}
	}
}

func TestManMissingTopicDoesNotPrintErrorBody(t *testing.T) {
	out := &bytes.Buffer{}
	app := &App{Stdout: out, API: &api.Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		response := jsonResponse("not a manual")
		response.StatusCode = 404
		return response, nil
	})}}}
	if err := app.Run(context.Background(), []string{"man", "js.missing"}); err == nil {
		t.Fatal("expected error")
	}
	if out.Len() != 0 {
		t.Fatalf("unexpected stdout: %q", out.String())
	}
}
