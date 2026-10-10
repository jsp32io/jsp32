package api

import (
	"context"
	"net/http"
	"testing"
)

func TestSystemInfoPreservesFullResponse(t *testing.T) {
	const body = `{"uptimeMs":1234,"board":"ESP32-S3 N16R8","firmware":{"version":"abc"},"futureCounter":9007199254740993}`
	for _, base := range []string{"http://device", "http://device/api/v1"} {
		client := &Client{BaseURL: base, HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
			if r.Method != http.MethodGet || r.URL.Path != "/api/v1/system/info" {
				t.Errorf("request: %s %s", r.Method, r.URL)
			}
			return jsonResponse(200, body), nil
		})}}
		info, err := client.System().Info(context.Background())
		if err != nil {
			t.Fatal(err)
		}
		if info.UptimeMs != 1234 || string(info.Raw) != body {
			t.Fatalf("response: %+v", info)
		}
	}
}

func TestSystemInfoInvalidResponses(t *testing.T) {
	for _, body := range []string{`{`, `null`, `{}`, `{"uptimeMs":null}`, `{"uptimeMs":-1}`, `{"uptimeMs":"1"}`} {
		client := &Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) { return jsonResponse(200, body), nil })}}
		if _, err := client.System().Info(context.Background()); err == nil {
			t.Errorf("accepted %s", body)
		}
	}
	client := &Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) { return jsonResponse(503, `unavailable`), nil })}}
	if _, err := client.System().Info(context.Background()); err == nil {
		t.Fatal("accepted HTTP error")
	}
}
