package command

import (
	"bytes"
	"context"
	"net/http"
	"strings"
	"testing"

	"github.com/jsp32io/jsp32/cli/api"
)

func TestRebootIsTopLevelCommand(t *testing.T) {
	called := 0
	stdout := &bytes.Buffer{}
	app := &App{
		API: &api.Client{BaseURL: "http://jsp32.local", HTTPClient: &http.Client{
			Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
				called++
				if request.Method != http.MethodPost || request.URL.Path != "/api/v1/ota/reboot" {
					t.Errorf("request = %s %s", request.Method, request.URL.Path)
				}
				return jsonResponse(`{"ok":true,"state":"rebooting"}`), nil
			}),
		}},
		Stdout: stdout,
		Stderr: &bytes.Buffer{},
	}
	if err := app.Run(context.Background(), []string{"help"}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(stdout.String(), "reboot") {
		t.Errorf("help does not list reboot: %q", stdout.String())
	}
	stdout.Reset()
	if err := app.Run(context.Background(), []string{"reboot"}); err != nil {
		t.Fatal(err)
	}
	if got := stdout.String(); got != "rebooting\n" {
		t.Errorf("stdout = %q", got)
	}
	for _, args := range [][]string{{"reboot", "extra"}, {"ota", "reboot"}} {
		if _, ok := app.Run(context.Background(), args).(*UsageError); !ok {
			t.Errorf("args %v did not return UsageError", args)
		}
	}
	if called != 1 {
		t.Errorf("reboot requests = %d, want 1", called)
	}
}

func TestRebootWaitsForNewBoot(t *testing.T) {
	postCount := 0
	probeCount := 0
	stdout := &bytes.Buffer{}
	app := &App{
		API: &api.Client{BaseURL: "http://jsp32.local", HTTPClient: &http.Client{
			Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
				switch request.URL.Path {
				case "/api/v1/system/info":
					probeCount++
					if probeCount < 3 {
						return jsonResponse(`{"uptimeMs":10000}`), nil
					}
					return jsonResponse(`{"uptimeMs":200}`), nil
				case "/api/v1/ota/reboot":
					postCount++
					return jsonResponse(`{"ok":true,"state":"rebooting"}`), nil
				default:
					t.Fatalf("unexpected path %q", request.URL.Path)
					return nil, nil
				}
			}),
		}},
		Stdout: stdout,
		Stderr: &bytes.Buffer{},
	}
	if err := app.Run(context.Background(), []string{"reboot", "-wait"}); err != nil {
		t.Fatal(err)
	}
	if got := stdout.String(); got != "rebooted\n" {
		t.Errorf("stdout = %q", got)
	}
	if postCount != 1 || probeCount != 3 {
		t.Errorf("POST count = %d, probe count = %d", postCount, probeCount)
	}
}
