package command

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"

	"github.com/jsp32io/jsp32/cli/api"
)

func nvsTestApp(handler roundTripFunc) (*App, *bytes.Buffer, *bytes.Buffer) {
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	return &App{API: &api.Client{BaseURL: "http://jsp32.local", HTTPClient: &http.Client{Transport: handler}}, Stdin: strings.NewReader(""), Stdout: stdout, Stderr: stderr, Commands: []Command{&NVSCommand{}}}, stdout, stderr
}

func TestNVSCommandNamespacesPaginates(t *testing.T) {
	calls := 0
	app, stdout, _ := nvsTestApp(func(request *http.Request) (*http.Response, error) {
		calls++
		if request.URL.Query().Get("prefix") != "user" {
			t.Errorf("prefix = %q", request.URL.Query().Get("prefix"))
		}
		if calls == 1 {
			return jsonResponse(`{"ok":true,"data":{"namespaces":[{"name":"user","policy":"user","usedEntries":2,"writable":true}],"hasMore":true,"nextAfter":"user"}}`), nil
		}
		if request.URL.Query().Get("after") != "user" {
			t.Errorf("after = %q", request.URL.Query().Get("after"))
		}
		return jsonResponse(`{"ok":true,"data":{"namespaces":[{"name":"user_cache","policy":"user","usedEntries":1,"writable":true}],"hasMore":false,"nextAfter":null}}`), nil
	})
	if err := app.Run(context.Background(), []string{"nvs", "namespaces", "--prefix", "user"}); err != nil {
		t.Fatal(err)
	}
	if got := stdout.String(); got != "2\ttrue\tuser\tuser\n1\ttrue\tuser\tuser_cache\n" {
		t.Errorf("stdout = %q", got)
	}
}

func TestNVSCommandGetJSONAndRawBlob(t *testing.T) {
	calls := 0
	app, stdout, _ := nvsTestApp(func(request *http.Request) (*http.Response, error) {
		calls++
		if calls == 1 {
			return jsonResponse(`{"ok":true,"data":{"namespace":"user","key":"count","type":"u32","value":42,"etag":"tag"}}`), nil
		}
		return jsonResponse(`{"ok":true,"data":{"namespace":"user","key":"data","type":"blob","encoding":"base64","value":"AAH/","etag":"tag"}}`), nil
	})
	if err := app.Run(context.Background(), []string{"nvs", "get", "user", "count"}); err != nil {
		t.Fatal(err)
	}
	var output map[string]any
	if err := json.Unmarshal(stdout.Bytes(), &output); err != nil {
		t.Fatal(err)
	}
	if output["type"] != "u32" || output["value"] != float64(42) {
		t.Errorf("output = %#v", output)
	}
	stdout.Reset()
	if err := app.Run(context.Background(), []string{"nvs", "get", "--raw", "user", "data"}); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(stdout.Bytes(), []byte{0, 1, 255}) {
		t.Errorf("stdout = %v", stdout.Bytes())
	}
}

func TestNVSCommandSetParsesTypesAndConditions(t *testing.T) {
	app, stdout, _ := nvsTestApp(func(request *http.Request) (*http.Response, error) {
		if request.Header.Get("If-None-Match") != "*" {
			t.Errorf("If-None-Match = %q", request.Header.Get("If-None-Match"))
		}
		var body struct {
			Type  string `json:"type"`
			Value string `json:"value"`
		}
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Fatal(err)
		}
		if body.Type != "u64" || body.Value != "9007199254740993" {
			t.Errorf("body = %+v", body)
		}
		return jsonResponse(`{"ok":true,"data":{"namespace":"user","key":"serial","type":"u64","value":"9007199254740993","etag":"tag"}}`), nil
	})
	if err := app.Run(context.Background(), []string{"nvs", "set", "--create", "user", "serial", "u64", "9007199254740993"}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(stdout.String(), `"type":"u64"`) {
		t.Errorf("stdout = %q", stdout.String())
	}
}

func TestNVSCommandSetBlobReadsStdin(t *testing.T) {
	app, _, _ := nvsTestApp(func(request *http.Request) (*http.Response, error) {
		var body map[string]any
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Fatal(err)
		}
		if body["encoding"] != "base64" || body["value"] != "AAH/" {
			t.Errorf("body = %#v", body)
		}
		return jsonResponse(`{"ok":true,"data":{"namespace":"user","key":"data","type":"blob","encoding":"base64","value":"AAH/","etag":"tag"}}`), nil
	})
	app.Stdin = bytes.NewReader([]byte{0, 1, 255})
	if err := app.Run(context.Background(), []string{"nvs", "set", "user", "data", "blob", "-"}); err != nil {
		t.Fatal(err)
	}
}

func TestNVSCommandBatchDefaultsToStdin(t *testing.T) {
	app, stdout, _ := nvsTestApp(func(request *http.Request) (*http.Response, error) {
		body, _ := io.ReadAll(request.Body)
		if string(body) != `{"operations":[{"op":"delete","key":"old"}]}` {
			t.Errorf("body = %s", body)
		}
		return jsonResponse(`{"ok":true,"data":{"namespace":"user","operations":[{"op":"delete","key":"old","deleted":true}]}}`), nil
	})
	app.Stdin = strings.NewReader(`{"operations":[{"op":"delete","key":"old"}]}`)
	if err := app.Run(context.Background(), []string{"nvs", "batch", "user"}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(stdout.String(), `"deleted":true`) {
		t.Errorf("stdout = %q", stdout.String())
	}
}

func TestNVSCommandClearRequiresConfirmation(t *testing.T) {
	app, stdout, _ := nvsTestApp(func(request *http.Request) (*http.Response, error) {
		if request.Header.Get("X-Confirm-Namespace") != "user" {
			t.Errorf("confirmation = %q", request.Header.Get("X-Confirm-Namespace"))
		}
		return jsonResponse(""), nil
	})
	err := app.Run(context.Background(), []string{"nvs", "clear", "user"})
	if _, ok := err.(*UsageError); !ok {
		t.Fatalf("error = %T %v", err, err)
	}
	if err := app.Run(context.Background(), []string{"nvs", "clear", "--yes", "user"}); err != nil {
		t.Fatal(err)
	}
	if stdout.String() != "user\n" {
		t.Errorf("stdout = %q", stdout.String())
	}
}

func TestNVSCommandHelpAndValidation(t *testing.T) {
	app := &App{Stdin: strings.NewReader(""), Stdout: &bytes.Buffer{}, Stderr: &bytes.Buffer{}, Commands: []Command{&NVSCommand{}}}
	if err := app.Run(context.Background(), []string{"nvs", "get", "--help"}); err != nil {
		t.Fatal(err)
	}
	for _, args := range [][]string{{"nvs", "get", "user"}, {"nvs", "set", "user", "key", "u8", "256"}, {"nvs", "clear", "user"}} {
		if _, ok := app.Run(context.Background(), args).(*UsageError); !ok {
			t.Errorf("args %v did not return UsageError", args)
		}
	}
}
