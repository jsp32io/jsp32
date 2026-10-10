package api

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
)

func TestRegWatchReadsNativeChangeEvent(t *testing.T) {
	client := &Client{BaseURL: "http://jsp32.local", HTTPClient: &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.URL.Path != "/api/v1/registers/events" || request.Header.Get("Accept") != "text/event-stream" {
			t.Errorf("unexpected request: %s %s", request.URL.Path, request.Header.Get("Accept"))
		}
		if request.URL.Query().Get("addresses") != "M10,D100" {
			t.Errorf("unexpected subscription: %s", request.URL.RawQuery)
		}
		return &http.Response{StatusCode: 200, Status: "200 OK", Header: make(http.Header), Body: io.NopCloser(strings.NewReader(": connected\n\nevent: change\ndata: {\"r\":\"5\",\"a\":\"M10\",\"v\":true}\n\n"))}, nil
	})}}
	stream, err := client.Reg().Watch(context.Background(), "M10", "D100")
	if err != nil {
		t.Fatal(err)
	}
	defer stream.Close()
	event, err := stream.Next()
	if err != nil {
		t.Fatal(err)
	}
	var change RegChange
	if err := json.Unmarshal([]byte(event.Data), &change); err != nil {
		t.Fatal(err)
	}
	if event.Type != "change" || change.Revision != "5" || change.Address != "M10" || string(change.Value) != "true" {
		t.Fatalf("unexpected event: %+v", event)
	}
}
