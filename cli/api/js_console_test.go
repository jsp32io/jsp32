package api

import (
	"context"
	"io"
	"net/http"
	"strings"
	"testing"
)

func TestJSConsoleReadsSSEEvents(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.Method != http.MethodGet {
			t.Errorf("method = %q, want GET", request.Method)
		}
		if request.URL.Path != "/api/v1/quickjs/console/events" {
			t.Errorf("path = %q, want console events path", request.URL.Path)
		}
		if got := request.Header.Get("Accept"); got != "text/event-stream" {
			t.Errorf("Accept = %q, want text/event-stream", got)
		}
		return &http.Response{
			StatusCode: http.StatusOK,
			Status:     "200 OK",
			Header:     make(http.Header),
			Body: io.NopCloser(strings.NewReader(
				": connected\n\nevent: console\ndata: first\ndata: second\n\n" +
					"event: dropped\ndata: 3\n\n")),
		}, nil
	})}

	client := &Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}
	stream, err := client.JS().Console(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	defer stream.Close()

	event, err := stream.Next()
	if err != nil {
		t.Fatal(err)
	}
	if event.Type != "console" || event.Data != "first\nsecond" {
		t.Errorf("event = %+v", event)
	}
	event, err = stream.Next()
	if err != nil {
		t.Fatal(err)
	}
	if event.Type != "dropped" || event.Data != "3" {
		t.Errorf("event = %+v", event)
	}
	if _, err := stream.Next(); err != io.EOF {
		t.Errorf("final error = %v, want EOF", err)
	}
}
