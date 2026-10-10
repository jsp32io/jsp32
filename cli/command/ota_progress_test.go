package command

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/jsp32io/jsp32/cli/api"
)

func TestOTAProgressUsesDeviceWrites(t *testing.T) {
	for _, failure := range []bool{false, true} {
		t.Run(fmt.Sprint("failure=", failure), func(t *testing.T) {
			const id = "0123456789abcdef0123456789abcdef"
			bodyRead := make(chan struct{})
			finish := make(chan struct{})
			polls := 0
			var finishOnce sync.Once
			client := &api.Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
				if r.Method == http.MethodPut {
					if r.Header.Get("X-Upload-ID") != id {
						t.Errorf("missing upload ID")
					}
					_, _ = io.Copy(io.Discard, r.Body)
					close(bodyRead) // All source bytes consumed before the first status poll.
					select {
					case <-finish:
					case <-r.Context().Done():
						return nil, r.Context().Err()
					}
					response := jsonResponse(`{"ok":true,"state":"ready","firmware":{"partition":"ota_1"}}`)
					if failure {
						response.StatusCode = 422
						response.Status = "422 Unprocessable Entity"
					}
					return response, nil
				}
				<-bodyRead
				polls++
				switch polls {
				case 1:
					return jsonResponse(`{"ok":true,"state":"receiving","upload":{"id":"someone-else","total":1000,"written":800}}`), nil
				case 2:
					return jsonResponse(`{"ok":true,"state":"receiving","upload":{"id":"` + id + `","total":1000,"received":1000,"written":0}}`), nil
				case 3:
					return jsonResponse(`{"ok":true,"state":"receiving","upload":{"id":"` + id + `","total":1000,"received":1000,"written":500}}`), nil
				case 4:
					return jsonResponse(`{"ok":true,"state":"verifying","upload":{"id":"` + id + `","total":1000,"received":1000,"written":1000}}`), nil
				default:
					finishOnce.Do(func() { close(finish) })
					return jsonResponse(`{"ok":true,"state":"ready","upload":null}`), nil
				}
			})}}
			output := &bytes.Buffer{}
			ctx, cancel := context.WithTimeout(context.Background(), time.Second)
			defer cancel()
			_, err := uploadWithDeviceProgress(ctx, client.OTA(), strings.NewReader(strings.Repeat("x", 1000)), 1000, strings.Repeat("a", 64), id, output, time.Millisecond)
			if (err != nil) != failure {
				t.Fatalf("error: %v", err)
			}
			got := output.String()
			for _, want := range []string{"0/1000 bytes", "50.0%  500/1000 bytes", "verifying..."} {
				if !strings.Contains(got, want) {
					t.Errorf("missing %q: %q", want, got)
				}
			}
			if strings.Contains(got, "800/1000") {
				t.Fatal("rendered another upload's progress")
			}
			if failure && strings.Contains(got, "100.0%") {
				t.Fatal("failed upload shown as complete")
			}
			if !failure && !strings.Contains(got, "ready  100.0%") {
				t.Fatal("missing completion")
			}
		})
	}
}

func TestOTAProgressCancelsProbeOnCompletion(t *testing.T) {
	probing := make(chan struct{})
	var probeOnce sync.Once
	client := &api.Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.Method == http.MethodPut {
			<-probing
			return jsonResponse(`{"ok":true,"state":"ready","firmware":{}}`), nil
		}
		probeOnce.Do(func() { close(probing) })
		<-r.Context().Done()
		return nil, r.Context().Err()
	})}}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	if _, err := uploadWithDeviceProgress(ctx, client.OTA(), strings.NewReader("x"), 1, strings.Repeat("a", 64), strings.Repeat("b", 32), io.Discard, time.Millisecond); err != nil {
		t.Fatal(err)
	}
}

func TestOTAProgressCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	client := &api.Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		cancel()
		<-r.Context().Done()
		return nil, r.Context().Err()
	})}}
	out := &bytes.Buffer{}
	_, err := uploadWithDeviceProgress(ctx, client.OTA(), strings.NewReader("x"), 1, strings.Repeat("a", 64), strings.Repeat("b", 32), out, time.Millisecond)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("error: %v", err)
	}
	if strings.Contains(out.String(), "100.0%") {
		t.Fatal("cancelled upload completed")
	}
}

func TestOTAProgressUnavailableAndRecovery(t *testing.T) {
	const id = "0123456789abcdef0123456789abcdef"
	finish := make(chan struct{})
	var once sync.Once
	polls := 0
	client := &api.Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		if r.Method == http.MethodPut {
			select {
			case <-finish:
			case <-r.Context().Done():
				return nil, r.Context().Err()
			}
			return jsonResponse(`{"ok":true,"state":"ready"}`), nil
		}
		polls++
		switch polls {
		case 1:
			resp := jsonResponse(`{}`)
			resp.StatusCode = 503
			return resp, nil
		case 2:
			return jsonResponse(`{"ok":true,"state":"receiving","upload":{"received":900,"total":1000}}`), nil
		case 3:
			return jsonResponse(`{"ok":true,"state":"receiving","upload":{"id":"` + id + `","written":500,"total":1000}}`), nil
		default:
			once.Do(func() { close(finish) })
			return jsonResponse(`{"ok":true,"state":"idle"}`), nil
		}
	})}}
	out := &bytes.Buffer{}
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_, err := uploadWithDeviceProgress(ctx, client.OTA(), strings.NewReader(strings.Repeat("x", 1000)), 1000, strings.Repeat("a", 64), id, out, time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{"progress unavailable", "progress unsupported", "50.0%", "ready  100.0%"} {
		if !strings.Contains(out.String(), want) {
			t.Errorf("missing %q: %q", want, out.String())
		}
	}
	if strings.Contains(out.String(), "90.0%") {
		t.Fatal("received progress shown as Flash progress")
	}
}
