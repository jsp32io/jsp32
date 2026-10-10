package command

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/jsp32io/jsp32/cli/api"
)

func TestConsoleCommandPrintsEventsAndDroppedWarning(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		return &http.Response{
			StatusCode: http.StatusOK,
			Status:     "200 OK",
			Header:     make(http.Header),
			Body: io.NopCloser(strings.NewReader(
				"event: console\ndata: hello\n\n" +
					"event: dropped\ndata: 2\n\n" +
					"event: console\ndata: world\n\n")),
		}, nil
	})}

	var stdout bytes.Buffer
	var stderr bytes.Buffer
	app := &App{
		API:      &api.Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient},
		Stdout:   &stdout,
		Stderr:   &stderr,
		Commands: []Command{&ConsoleCommand{}},
	}
	if err := app.Run(context.Background(), []string{"console"}); err != nil {
		t.Fatal(err)
	}
	if got := stdout.String(); got != "hello\nworld\n" {
		t.Errorf("stdout = %q", got)
	}
	if got := stderr.String(); got != "warning: 2 console messages dropped\n" {
		t.Errorf("stderr = %q", got)
	}
}

func TestConsoleCommandRejectsArguments(t *testing.T) {
	app := &App{
		Stdout:   &bytes.Buffer{},
		Stderr:   &bytes.Buffer{},
		Commands: []Command{&ConsoleCommand{}},
	}
	err := app.Run(context.Background(), []string{"console", "extra"})
	if _, ok := err.(*UsageError); !ok {
		t.Fatalf("error = %T %v, want *UsageError", err, err)
	}
}
