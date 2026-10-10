package command

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/jsp32io/jsp32/cli/api"
)

const commandTestSHA256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

func fsTestApp(handler roundTripFunc) (*App, *bytes.Buffer, *bytes.Buffer) {
	stdout := &bytes.Buffer{}
	stderr := &bytes.Buffer{}
	return &App{
		API:      &api.Client{BaseURL: "http://jsp32.local", HTTPClient: &http.Client{Transport: handler}},
		Stdout:   stdout,
		Stderr:   stderr,
		Commands: []Command{&FSCommand{}},
	}, stdout, stderr
}

func TestFSCommandListPaginates(t *testing.T) {
	calls := 0
	app, stdout, _ := fsTestApp(func(request *http.Request) (*http.Response, error) {
		calls++
		if request.URL.Query().Get("limit") != "100" {
			t.Errorf("limit = %q", request.URL.Query().Get("limit"))
		}
		if calls == 1 {
			return jsonResponse(`{"ok":true,"files":[{"path":"/a","size":1}],"nextAfter":"/a"}`), nil
		}
		if request.URL.Query().Get("after") != "/a" {
			t.Errorf("after = %q", request.URL.Query().Get("after"))
		}
		return jsonResponse(`{"ok":true,"files":[{"path":"/b","size":2}],"nextAfter":null}`), nil
	})
	if err := app.Run(context.Background(), []string{"fs", "ls"}); err != nil {
		t.Fatal(err)
	}
	if got := stdout.String(); got != "1\t/a\n2\t/b\n" {
		t.Errorf("stdout = %q", got)
	}
}

func TestFSCommandGetDefaultsToStdout(t *testing.T) {
	data := []byte{0, 1, 2, 255}
	checksum := "3d1f57c984978ef98a18378c8166c1cb8ede02c03eeb6aee7e2f121dfeee3e56"
	app, stdout, _ := fsTestApp(func(request *http.Request) (*http.Response, error) {
		header := make(http.Header)
		header.Set("ETag", `"sha256-`+checksum+`"`)
		header.Set("X-File-Size", "4")
		return &http.Response{StatusCode: http.StatusOK, Status: "200 OK", Header: header, Body: io.NopCloser(bytes.NewReader(data))}, nil
	})
	if err := app.Run(context.Background(), []string{"fs", "get", "/data"}); err != nil {
		t.Fatal(err)
	}
	if !bytes.Equal(stdout.Bytes(), data) {
		t.Errorf("stdout = %v", stdout.Bytes())
	}
}

func TestFSCommandDirectories(t *testing.T) {
	directories := map[string]bool{"/tree": true, "/tree/sub": true}
	files := map[string]bool{"/tree/a.txt": true, "/tree/sub/b.txt": true}
	app, stdout, _ := fsTestApp(func(request *http.Request) (*http.Response, error) {
		path := request.URL.Query().Get("path")
		switch {
		case request.Method == http.MethodPost && request.URL.Path == "/api/v1/fs/directory":
			directories[path] = true
			return jsonResponse(`{"ok":true,"path":"` + path + `"}`), nil
		case request.Method == http.MethodGet && request.URL.Path == "/api/v1/fs/files":
			switch path {
			case "/tree":
				if directories["/tree/sub"] {
					return jsonResponse(`{"ok":true,"files":[{"path":"/tree/a.txt","size":1},{"path":"/tree/sub","size":0,"directory":true}],"nextAfter":null}`), nil
				}
				return jsonResponse(`{"ok":true,"files":[],"nextAfter":null}`), nil
			case "/tree/sub":
				if files["/tree/sub/b.txt"] {
					return jsonResponse(`{"ok":true,"files":[{"path":"/tree/sub/b.txt","size":1}],"nextAfter":null}`), nil
				}
				return jsonResponse(`{"ok":true,"files":[],"nextAfter":null}`), nil
			}
		case request.Method == http.MethodDelete && request.URL.Path == "/api/v1/fs/file":
			if path == "/tree" && (files["/tree/a.txt"] || directories["/tree/sub"]) ||
				path == "/tree/sub" && files["/tree/sub/b.txt"] {
				response := jsonResponse(`{"ok":false,"error":{"code":"FS_DIRECTORY_NOT_EMPTY","name":"StateError","message":"directory is not empty"}}`)
				response.StatusCode = http.StatusConflict
				response.Status = "409 Conflict"
				return response, nil
			}
			delete(files, path)
			delete(directories, path)
			response := jsonResponse("")
			response.StatusCode = http.StatusNoContent
			response.Status = "204 No Content"
			return response, nil
		}
		t.Fatalf("unexpected request: %s %s", request.Method, request.URL)
		return nil, nil
	})
	if err := app.Run(context.Background(), []string{"fs", "mkdir", "/new"}); err != nil {
		t.Fatal(err)
	}
	stdout.Reset()
	if err := app.Run(context.Background(), []string{"fs", "ls", "/tree"}); err != nil {
		t.Fatal(err)
	}
	if got := stdout.String(); got != "1\t/tree/a.txt\ndir\t/tree/sub\n" {
		t.Errorf("ls output = %q", got)
	}
	if err := app.Run(context.Background(), []string{"fs", "rm", "/tree"}); err == nil {
		t.Fatal("non-empty directory was removed without -r")
	}
	if err := app.Run(context.Background(), []string{"fs", "rm", "-r", "/tree"}); err != nil {
		t.Fatal(err)
	}
	if len(files) != 0 || directories["/tree"] || directories["/tree/sub"] || !directories["/new"] {
		t.Errorf("remaining files = %v, directories = %v", files, directories)
	}
}

func TestFSCommandGetReplacesLocalFile(t *testing.T) {
	directory := t.TempDir()
	target := filepath.Join(directory, "main.js")
	if err := os.WriteFile(target, []byte("old"), 0o600); err != nil {
		t.Fatal(err)
	}
	const newSHA256 = "11507a0e2f5e69d5dfa40a62a1bd7b6ee57e6bcd85c67c9b8431b36fff21c437"
	app, stdout, _ := fsTestApp(func(request *http.Request) (*http.Response, error) {
		header := make(http.Header)
		header.Set("ETag", `"sha256-`+newSHA256+`"`)
		header.Set("X-File-Size", "3")
		return &http.Response{StatusCode: http.StatusOK, Status: "200 OK", Header: header, Body: io.NopCloser(strings.NewReader("new"))}, nil
	})
	if err := app.Run(context.Background(), []string{"fs", "get", "/main.js", target}); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(target)
	if err != nil {
		t.Fatal(err)
	}
	if string(data) != "new" || !strings.Contains(stdout.String(), newSHA256) {
		t.Errorf("data = %q, stdout = %q", data, stdout.String())
	}
	matches, _ := filepath.Glob(filepath.Join(directory, ".jsp32-download-*"))
	if len(matches) != 0 {
		t.Errorf("temporary files remain: %v", matches)
	}
}

func TestFSCommandPutHashesAndWarnsForMain(t *testing.T) {
	directory := t.TempDir()
	source := filepath.Join(directory, "main.js")
	if err := os.WriteFile(source, []byte("abc"), 0o600); err != nil {
		t.Fatal(err)
	}
	app, stdout, stderr := fsTestApp(func(request *http.Request) (*http.Response, error) {
		if request.Header.Get("If-None-Match") != "*" {
			t.Errorf("If-None-Match = %q", request.Header.Get("If-None-Match"))
		}
		if got := request.Header.Get("X-Content-SHA256"); got != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" {
			t.Errorf("checksum = %q", got)
		}
		return jsonResponse(`{"ok":true,"file":{"path":"/main.js","size":3,"sha256":"sha","created":true,"restartRequired":true}}`), nil
	})
	if err := app.Run(context.Background(), []string{"fs", "put", "--create", "/main.js", source}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(stdout.String(), "created\t/main.js\t3 bytes") {
		t.Errorf("stdout = %q", stdout.String())
	}
	if got := stderr.String(); got != "warning: restart the device to activate /main.js\n" {
		t.Errorf("stderr = %q", got)
	}
}

func TestFSCommandPutDefaultsToStdin(t *testing.T) {
	app, stdout, _ := fsTestApp(func(request *http.Request) (*http.Response, error) {
		if request.URL.Query().Get("path") != "/data" || request.ContentLength != 3 {
			t.Errorf("path = %q, length = %d", request.URL.Query().Get("path"), request.ContentLength)
		}
		data, _ := io.ReadAll(request.Body)
		if string(data) != "abc" {
			t.Errorf("body = %q", data)
		}
		return jsonResponse(`{"ok":true,"file":{"path":"/data","size":3,"sha256":"sha","created":true,"restartRequired":false}}`), nil
	})
	app.Stdin = strings.NewReader("abc")
	if err := app.Run(context.Background(), []string{"fs", "put", "/data"}); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(stdout.String(), "created\t/data\t3 bytes") {
		t.Errorf("stdout = %q", stdout.String())
	}
}

func TestFSCommandGetKeepsExistingFileOnChecksumMismatch(t *testing.T) {
	directory := t.TempDir()
	target := filepath.Join(directory, "data")
	if err := os.WriteFile(target, []byte("old"), 0o600); err != nil {
		t.Fatal(err)
	}
	app, _, _ := fsTestApp(func(request *http.Request) (*http.Response, error) {
		header := make(http.Header)
		header.Set("ETag", `"sha256-`+commandTestSHA256+`"`)
		header.Set("X-File-Size", "3")
		return &http.Response{StatusCode: http.StatusOK, Status: "200 OK", Header: header, Body: io.NopCloser(strings.NewReader("new"))}, nil
	})
	err := app.Run(context.Background(), []string{"fs", "get", "/data", target})
	if err == nil || !strings.Contains(err.Error(), "SHA-256 mismatch") {
		t.Fatalf("error = %v", err)
	}
	data, readErr := os.ReadFile(target)
	if readErr != nil {
		t.Fatal(readErr)
	}
	if string(data) != "old" {
		t.Errorf("existing file changed to %q", data)
	}
}

func TestFSCommandValidatesArguments(t *testing.T) {
	app := &App{Stdout: &bytes.Buffer{}, Stderr: &bytes.Buffer{}, Commands: []Command{&FSCommand{}}}
	for _, args := range [][]string{
		{"fs", "get"},
		{"fs", "get", "/x", "a", "extra"},
		{"fs", "put", "--create", "--if-match", commandTestSHA256, "/a", "a"},
		{"fs", "rm", "--if-match", "bad", "/a"},
	} {
		err := app.Run(context.Background(), args)
		if _, ok := err.(*UsageError); !ok {
			t.Errorf("args %v: error = %T %v, want *UsageError", args, err, err)
		}
	}
}
