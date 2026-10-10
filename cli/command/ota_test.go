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

func otaTestApp(handler roundTripFunc) (*App, *bytes.Buffer, *bytes.Buffer) {
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	return &App{API: &api.Client{BaseURL: "http://jsp32.local", HTTPClient: &http.Client{Transport: handler}}, Stdin: strings.NewReader(""), Stdout: stdout, Stderr: stderr, Commands: []Command{&OTACommand{}}}, stdout, stderr
}

func TestOTACommandStatus(t *testing.T) {
	app, stdout, _ := otaTestApp(func(request *http.Request) (*http.Response, error) {
		return jsonResponse(`{"ok":true,"state":"pending_confirm","running":{"partition":"ota_1","address":4259840,"size":4194304,"version":"v2","state":"pending_verify"},"boot":{"partition":"ota_1","address":4259840,"size":4194304},"next":{"partition":"ota_0","address":65536,"size":4194304},"upload":null,"rollback":{"enabled":true,"possible":true},"lastError":null}`), nil
	})
	if err := app.Run(context.Background(), []string{"ota", "status"}); err != nil {
		t.Fatal(err)
	}
	for _, expected := range []string{"state\tpending_confirm", "running\tota_1", "rollback-possible\ttrue"} {
		if !strings.Contains(stdout.String(), expected) {
			t.Errorf("stdout %q lacks %q", stdout.String(), expected)
		}
	}
}

func TestOTACommandUploadDefaultsToStdin(t *testing.T) {
	app, stdout, stderr := otaTestApp(func(request *http.Request) (*http.Response, error) {
		if request.ContentLength != 3 {
			t.Errorf("length = %d", request.ContentLength)
		}
		if got := request.Header.Get("X-Content-SHA256"); got != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" {
			t.Errorf("checksum = %q", got)
		}
		body, _ := io.ReadAll(request.Body)
		if string(body) != "abc" {
			t.Errorf("body = %q", body)
		}
		return jsonResponse(`{"ok":true,"state":"ready","sha256":"sha","firmware":{"partition":"ota_1","address":4259840,"size":4194304,"project":"jsp32","version":"v2"}}`), nil
	})
	app.Stdin = strings.NewReader("abc")
	if err := app.Run(context.Background(), []string{"ota", "upload"}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(stdout.String(), "ready\tota_1\t3 bytes") {
		t.Errorf("stdout = %q", stdout.String())
	}
	if !strings.Contains(stderr.String(), "jsp32 reboot") {
		t.Errorf("stderr = %q", stderr.String())
	}
	if !strings.Contains(stderr.String(), "ready  100.0%") {
		t.Errorf("stderr = %q", stderr.String())
	}
}

func TestOTACommandUploadProgressEndsLineOnError(t *testing.T) {
	app, _, stderr := otaTestApp(func(request *http.Request) (*http.Response, error) {
		_, _ = io.ReadAll(request.Body)
		response := jsonResponse(`{"ok":false,"error":{"code":"OTA_INVALID_FIRMWARE","name":"FirmwareError","message":"firmware image is invalid"}}`)
		response.StatusCode = http.StatusUnprocessableEntity
		response.Status = "422 Unprocessable Entity"
		return response, nil
	})
	app.Stdin = strings.NewReader("abc")
	if err := app.Run(context.Background(), []string{"ota", "upload"}); err == nil {
		t.Fatal("expected upload error")
	}
	if got := stderr.String(); !strings.HasSuffix(got, "\n") || strings.Contains(got, "100.0%") {
		t.Errorf("stderr = %q", got)
	}
}

func TestOTACommandActionsAndRollbackConfirmation(t *testing.T) {
	app, stdout, _ := otaTestApp(func(request *http.Request) (*http.Response, error) {
		switch request.URL.Path {
		case "/api/v1/ota/confirm":
			return jsonResponse(`{"ok":true,"state":"valid","changed":false}`), nil
		case "/api/v1/ota/rollback":
			return jsonResponse(`{"ok":true,"state":"rolling_back"}`), nil
		default:
			t.Fatalf("path = %q", request.URL.Path)
			return nil, nil
		}
	})
	if err := app.Run(context.Background(), []string{"ota", "confirm"}); err != nil {
		t.Fatal(err)
	}
	err := app.Run(context.Background(), []string{"ota", "rollback"})
	if _, ok := err.(*UsageError); !ok {
		t.Fatalf("error = %T %v", err, err)
	}
	if err := app.Run(context.Background(), []string{"ota", "rollback", "--yes"}); err != nil {
		t.Fatal(err)
	}
	if got := stdout.String(); got != "valid\tchanged=false\nrolling_back\n" {
		t.Errorf("stdout = %q", got)
	}
}

func TestOTACommandValidatesArguments(t *testing.T) {
	app := &App{Stdin: strings.NewReader(""), Stdout: &bytes.Buffer{}, Stderr: &bytes.Buffer{}, Commands: []Command{&OTACommand{}}}
	for _, args := range [][]string{{"ota", "status", "extra"}, {"ota", "upload", "a", "b"}, {"ota", "confirm", "extra"}} {
		if _, ok := app.Run(context.Background(), args).(*UsageError); !ok {
			t.Errorf("args %v did not return UsageError", args)
		}
	}
	if err := app.Run(context.Background(), []string{"ota", "rollback", "--help"}); err != nil {
		t.Fatal(err)
	}
}
