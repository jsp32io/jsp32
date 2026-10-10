package api

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"testing"
)

func TestManualMarkdownAndVersion(t *testing.T) {
	for _, base := range []string{"http://device", "http://device/api/v1"} {
		for _, topic := range []string{"", "index", "js.index", "js.gpio"} {
			wantTopic := topic
			if wantTopic == "" {
				wantTopic = "index"
			}
			markdown := "# GPIO 手册\n\n```js\ngpio.list();\n```\n"
			c := &Client{BaseURL: base, HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
				if r.Method != "GET" || r.URL.Path != "/api/v1/man/"+wantTopic || r.Header.Get("Accept") != "text/markdown" {
					t.Errorf("unexpected request: %s %s", r.Method, r.URL)
				}
				return jsonResponse(200, markdown), nil
			})}}
			got, err := c.Manual(context.Background(), topic)
			if err != nil || got != markdown {
				t.Fatalf("got %q, %v", got, err)
			}
		}
	}
}

func TestManualErrors(t *testing.T) {
	c := &Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		t.Fatal("invalid topic made a request")
		return nil, nil
	})}}
	for _, topic := range []string{"../js/gpio", "js..gpio", "js/gpio", "js.gpio?x", "js.%67pio", ".js", "js.", "js gpio"} {
		if _, err := c.Manual(context.Background(), topic); err == nil {
			t.Errorf("accepted %q", topic)
		}
	}
	for _, status := range []int{404, 500} {
		c.HTTPClient.Transport = roundTripFunc(func(r *http.Request) (*http.Response, error) { return jsonResponse(status, "failure"), nil })
		if _, err := c.Manual(context.Background(), "js.missing"); err == nil || (status == 404 && !strings.Contains(err.Error(), "not found")) {
			t.Errorf("status %d: %v", status, err)
		}
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	c.HTTPClient.Transport = roundTripFunc(func(r *http.Request) (*http.Response, error) { return nil, r.Context().Err() })
	if _, err := c.Manual(ctx, "js.gpio"); !errors.Is(err, context.Canceled) {
		t.Fatalf("cancellation: %v", err)
	}
	var nilClient *Client
	if _, err := nilClient.Manual(context.Background(), "js.gpio"); err == nil {
		t.Fatal("nil client accepted")
	}
}
