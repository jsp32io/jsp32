package command

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"github.com/jsp32io/jsp32/cli/api"
	"net/http"
	"strings"
	"testing"
)

func TestInfoCommand(t *testing.T) {
	out := &bytes.Buffer{}
	app := &App{Stdout: out, API: &api.Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.Method != "GET" || r.URL.Path != "/api/v1/system/info" {
			t.Errorf("request: %s %s", r.Method, r.URL)
		}
		return jsonResponse(`{"board":"ESP32-S3 N16R8","uptimeMs":42,"heap":{"free":1024},"futureCounter":9007199254740993}`), nil
	})}}}
	if err := app.Run(context.Background(), []string{"info"}); err != nil {
		t.Fatal(err)
	}
	if !json.Valid(out.Bytes()) || !strings.Contains(out.String(), "\n  \"board\":") || !strings.Contains(out.String(), "9007199254740993") || !strings.Contains(out.String(), `"free": 1024`) {
		t.Fatalf("output: %s", out)
	}
}

func TestInfoUsage(t *testing.T) {
	out := &bytes.Buffer{}
	app := &App{Stdout: out}
	for _, args := range [][]string{{"help"}, {"info", "help"}, {"info", "--help"}, {"info", "-h"}} {
		out.Reset()
		if err := app.Run(context.Background(), args); err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(out.String(), "info") {
			t.Errorf("missing help: %s", out)
		}
	}
	for _, args := range [][]string{{"info", "extra"}, {"info", "--unknown"}} {
		var usage *UsageError
		if err := app.Run(context.Background(), args); !errors.As(err, &usage) {
			t.Errorf("expected usage error: %v", err)
		}
	}
	out.Reset()
	if err := app.Run(context.Background(), []string{"info"}); err == nil {
		t.Fatal("nil client accepted")
	}
	if out.Len() != 0 {
		t.Fatal("output on failure")
	}
}
