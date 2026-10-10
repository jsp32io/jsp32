package command

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"strings"
	"testing"

	"github.com/jsp32io/jsp32/cli/api"
)

func TestRegCommands(t *testing.T) {
	values := map[string]any{"M10": false, "D100": float64(0), "D101": float64(0)}
	transport := regRoundTrip(func(r *http.Request) (*http.Response, error) {
		w := &regResponseWriter{header: make(http.Header)}
		w.Header().Set("Content-Type", "application/json")
		switch r.URL.Path {
		case "/api/v1/registers":
			_, _ = w.Write([]byte(`{"revision":"0","areas":{"X":256,"Y":256,"M":1024,"D":1024},"maxTransaction":256,"volatile":true}`))
		case "/api/v1/registers/read":
			var body struct {
				Addresses []string `json:"addresses"`
				Start     string   `json:"start"`
				Count     int      `json:"count"`
			}
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
				t.Error(err)
			}
			if body.Start != "" {
				body.Addresses = []string{body.Start, "D101"}[:body.Count]
			}
			result := api.RegSnapshot{Revision: "1"}
			for _, address := range body.Addresses {
				raw, _ := json.Marshal(values[address])
				result.Values = append(result.Values, api.RegValue{Address: address, Value: raw})
			}
			_ = json.NewEncoder(w).Encode(result)
		case "/api/v1/registers/write":
			var body struct {
				Writes []api.RegWrite `json:"writes"`
				Start  string         `json:"start"`
				Values []any          `json:"values"`
			}
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
				t.Error(err)
			}
			for _, write := range body.Writes {
				values[write.Address] = write.Value
			}
			if body.Start != "" {
				for i, value := range body.Values {
					values["D"+stringNumber(100+i)] = value
				}
			}
			_, _ = w.Write([]byte(`{"revision":"1"}`))
		default:
			w.WriteHeader(http.StatusNotFound)
		}
		return &http.Response{StatusCode: w.status(), Status: http.StatusText(w.status()), Header: w.header, Body: io.NopCloser(bytes.NewReader(w.body.Bytes()))}, nil
	})
	var stdout, stderr bytes.Buffer
	app := &App{API: &api.Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: transport}}, Stdout: &stdout, Stderr: &stderr}
	for _, args := range [][]string{{"reg", "info"}, {"reg", "set", "M10=1", "D100=42"}, {"reg", "get", "M10", "D100"}, {"reg", "write", "D100", "10", "20"}, {"reg", "read", "D100", "2", "--json"}} {
		stdout.Reset()
		if err := app.Run(context.Background(), args); err != nil {
			t.Fatalf("%v: %v", args, err)
		}
	}
	if !strings.Contains(stdout.String(), `"address":"D101","value":20`) {
		t.Fatalf("unexpected read: %s", stdout.String())
	}
}

func TestRegRejectsInvalidWritesBeforeRequest(t *testing.T) {
	app := &App{API: &api.Client{BaseURL: "http://127.0.0.1:1"}, Stdout: &bytes.Buffer{}, Stderr: &bytes.Buffer{}}
	for _, args := range [][]string{{"reg", "set", "X0=1"}, {"reg", "set", "M01=1"}, {"reg", "set", "D0=65536"}, {"reg", "write", "M0", "2"}} {
		if err := app.Run(context.Background(), args); err == nil {
			t.Fatalf("accepted %v", args)
		}
	}
}

func TestRegWatchCompactEvents(t *testing.T) {
	const revision = "9007199254740993"
	for _, jsonOutput := range []bool{false, true} {
		t.Run(strconv.FormatBool(jsonOutput), func(t *testing.T) {
			transport := regRoundTrip(func(r *http.Request) (*http.Response, error) {
				if r.URL.Path != "/api/v1/registers/events" || r.URL.Query().Get("addresses") != "M10,D100" {
					t.Fatalf("unexpected subscription: %s", r.URL)
				}
				body := ": connected\n\nevent: change\ndata: {\"r\":\"" + revision + "\",\"a\":\"M10\",\"v\":true}\n\n" +
					"event: change\ndata: {\"r\":\"" + revision + "\",\"a\":\"D100\",\"v\":42}\n\n" +
					"event: dropped\ndata: 3\n\n: ping\n\n"
				return &http.Response{StatusCode: 200, Body: io.NopCloser(strings.NewReader(body))}, nil
			})
			var stdout, stderr bytes.Buffer
			app := &App{API: &api.Client{BaseURL: "http://device", HTTPClient: &http.Client{Transport: transport}}, Stdout: &stdout, Stderr: &stderr}
			args := []string{"reg", "watch", "M10", "D100"}
			if jsonOutput {
				args = append(args, "--json")
			}
			if err := app.Run(context.Background(), args); err != nil {
				t.Fatal(err)
			}
			want := revision + "\tM10\t1\n" + revision + "\tD100\t42\n"
			if jsonOutput {
				want = "{\"r\":\"" + revision + "\",\"a\":\"M10\",\"v\":true}\n" +
					"{\"r\":\"" + revision + "\",\"a\":\"D100\",\"v\":42}\n"
			}
			if stdout.String() != want || stderr.String() != "warning: 3 register changes dropped\n" {
				t.Fatalf("stdout=%q stderr=%q", stdout.String(), stderr.String())
			}
		})
	}
}

func stringNumber(value int) string { return strconv.Itoa(value) }

type regRoundTrip func(*http.Request) (*http.Response, error)

func (f regRoundTrip) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

type regResponseWriter struct {
	header http.Header
	body   bytes.Buffer
	code   int
}

func (w *regResponseWriter) Header() http.Header            { return w.header }
func (w *regResponseWriter) Write(data []byte) (int, error) { return w.body.Write(data) }
func (w *regResponseWriter) WriteHeader(code int)           { w.code = code }
func (w *regResponseWriter) status() int {
	if w.code == 0 {
		return 200
	}
	return w.code
}
