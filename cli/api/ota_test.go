package api

import (
	"context"
	"io"
	"net/http"
	"strings"
	"testing"
)

func TestOTAStatus(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.Method != http.MethodGet || request.URL.Path != "/api/v1/ota" {
			t.Errorf("request = %s %s", request.Method, request.URL.Path)
		}
		return jsonResponse(http.StatusOK, `{"ok":true,"state":"idle","running":{"partition":"ota_0","address":65536,"size":4194304,"project":"jsp32","version":"v1","secureVersion":0,"state":"valid"},"boot":{"partition":"ota_0","address":65536,"size":4194304},"next":{"partition":"ota_1","address":4259840,"size":4194304},"upload":null,"rollback":{"enabled":true,"possible":false},"lastError":null}`), nil
	})}
	status, err := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).OTA().Status(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if status.State != "idle" || status.Running.Partition != "ota_0" || status.Next.Partition != "ota_1" || !status.Rollback.Enabled {
		t.Errorf("status = %+v", status)
	}
}

func TestOTAUploadSetsLengthAndChecksum(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		if request.Method != http.MethodPut || request.URL.Path != "/api/v1/ota/firmware" || request.ContentLength != 3 {
			t.Errorf("request = %s %s length %d", request.Method, request.URL.Path, request.ContentLength)
		}
		if request.Header.Get("Content-Type") != "application/octet-stream" || request.Header.Get("X-Content-SHA256") != testSHA256 {
			t.Errorf("headers = %v", request.Header)
		}
		body, _ := io.ReadAll(request.Body)
		if string(body) != "abc" {
			t.Errorf("body = %q", body)
		}
		return jsonResponse(http.StatusOK, `{"ok":true,"state":"ready","sha256":"`+testSHA256+`","firmware":{"partition":"ota_1","address":4259840,"size":4194304,"project":"jsp32","version":"v2","secureVersion":0}}`), nil
	})}
	result, err := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).OTA().Upload(context.Background(), strings.NewReader("abc"), 3, testSHA256)
	if err != nil {
		t.Fatal(err)
	}
	if result.State != "ready" || result.Firmware.Partition != "ota_1" {
		t.Errorf("result = %+v", result)
	}
}

func TestOTAActions(t *testing.T) {
	calls := 0
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		calls++
		if request.Method != http.MethodPost || request.ContentLength != 0 {
			t.Errorf("request = %s length %d", request.Method, request.ContentLength)
		}
		switch request.URL.Path {
		case "/api/v1/ota/reboot":
			return jsonResponse(http.StatusAccepted, `{"ok":true,"state":"rebooting"}`), nil
		case "/api/v1/ota/confirm":
			return jsonResponse(http.StatusOK, `{"ok":true,"state":"valid","changed":true}`), nil
		case "/api/v1/ota/rollback":
			return jsonResponse(http.StatusAccepted, `{"ok":true,"state":"rolling_back"}`), nil
		default:
			t.Fatalf("path = %q", request.URL.Path)
			return nil, nil
		}
	})}
	ota := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).OTA()
	if result, err := ota.Reboot(context.Background()); err != nil || result.State != "rebooting" {
		t.Fatalf("reboot = %+v, %v", result, err)
	}
	if result, err := ota.Confirm(context.Background()); err != nil || result.Changed == nil || !*result.Changed {
		t.Fatalf("confirm = %+v, %v", result, err)
	}
	if result, err := ota.Rollback(context.Background()); err != nil || result.State != "rolling_back" {
		t.Fatalf("rollback = %+v, %v", result, err)
	}
	if calls != 3 {
		t.Errorf("calls = %d", calls)
	}
}

func TestOTAUploadValidationAndError(t *testing.T) {
	ota := (&Client{}).OTA()
	if _, err := ota.Upload(context.Background(), strings.NewReader(""), 0, testSHA256); err == nil {
		t.Fatal("expected empty firmware error")
	}
	if _, err := ota.Upload(context.Background(), strings.NewReader("x"), 1, "bad"); err == nil {
		t.Fatal("expected checksum error")
	}
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		return jsonResponse(http.StatusConflict, `{"ok":false,"error":{"code":"OTA_CONFIRM_REQUIRED","name":"StateError","message":"confirm first"}}`), nil
	})}
	_, err := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).OTA().Reboot(context.Background())
	httpError, ok := err.(*HTTPError)
	if !ok || httpError.Detail == nil || httpError.Detail.Code != "OTA_CONFIRM_REQUIRED" {
		t.Fatalf("error = %#v", err)
	}
}

func TestOTAUploadIDAndWrittenStatus(t *testing.T) {
	const id = "0123456789abcdef0123456789abcdef"
	client := &Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.Method == http.MethodPut {
			if r.Header.Get("X-Upload-ID") != id {
				t.Errorf("ID: %s", r.Header.Get("X-Upload-ID"))
			}
			return jsonResponse(200, `{"ok":true,"state":"ready"}`), nil
		}
		return jsonResponse(200, `{"ok":true,"state":"receiving","upload":{"id":"`+id+`","received":70000,"written":65536,"total":100000,"writtenPercent":65.536}}`), nil
	})}}
	if _, err := client.OTA().UploadWithID(context.Background(), strings.NewReader("abc"), 3, testSHA256, id); err != nil {
		t.Fatal(err)
	}
	status, err := client.OTA().Status(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if status.Upload.ID != id || status.Upload.Written == nil || *status.Upload.Written != 65536 {
		t.Fatalf("status: %+v", status.Upload)
	}
	for _, invalid := range []string{"", "short", strings.Repeat("z", 32)} {
		if _, err := client.OTA().UploadWithID(context.Background(), strings.NewReader("x"), 1, testSHA256, invalid); err == nil {
			t.Errorf("accepted %q", invalid)
		}
	}
}
