package api

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"testing"
)

func TestNVSInfoAndNamespacePagination(t *testing.T) {
	calls := 0
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		calls++
		switch request.URL.Path {
		case "/api/v1/nvs":
			return jsonResponse(http.StatusOK, `{"ok":true,"data":{"partition":"nvs","size":24576,"encrypted":false,"usedEntries":4,"freeEntries":10,"totalEntries":14,"namespaceCount":2,"limits":{"maxStringBytes":1024,"maxBlobBytes":4096,"maxBatchOperations":16}}}`), nil
		case "/api/v1/nvs/namespaces":
			if request.URL.Query().Get("prefix") != "user_" || request.URL.Query().Get("after") != "user_a" || request.URL.Query().Get("limit") != "2" {
				t.Errorf("query = %q", request.URL.RawQuery)
			}
			return jsonResponse(http.StatusOK, `{"ok":true,"data":{"namespaces":[{"name":"user_b","policy":"user","usedEntries":2,"writable":true}],"hasMore":false,"nextAfter":null}}`), nil
		default:
			t.Fatalf("unexpected path %q", request.URL.Path)
			return nil, nil
		}
	})}
	nvs := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).NVS()
	info, err := nvs.Info(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if info.Size != 24576 || info.Limits.MaxBatchOperations != 16 {
		t.Errorf("info = %+v", info)
	}
	page, err := nvs.ListNamespaces(context.Background(), "user_", 2, "user_a")
	if err != nil {
		t.Fatal(err)
	}
	if len(page.Namespaces) != 1 || page.Namespaces[0].Name != "user_b" || page.HasMore {
		t.Errorf("page = %+v", page)
	}
	if calls != 2 {
		t.Errorf("calls = %d", calls)
	}
}

func TestNVSGetAndSetTypedValue(t *testing.T) {
	calls := 0
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		calls++
		if request.URL.Path != "/api/v1/nvs/namespaces/user/entries/boot_count" {
			t.Errorf("path = %q", request.URL.Path)
		}
		if calls == 1 {
			if request.Method != http.MethodGet {
				t.Errorf("method = %s", request.Method)
			}
			return jsonResponse(http.StatusOK, `{"ok":true,"data":{"namespace":"user","key":"boot_count","type":"u32","value":41,"etag":"\"sha256-`+testSHA256+`\""}}`), nil
		}
		if request.Method != http.MethodPut || request.Header.Get("If-None-Match") != "*" || request.Header.Get("Content-Type") != "application/json" {
			t.Errorf("set request = %+v", request)
		}
		var body map[string]any
		if err := json.NewDecoder(request.Body).Decode(&body); err != nil {
			t.Fatal(err)
		}
		if body["type"] != "u32" || body["value"] != float64(42) {
			t.Errorf("body = %#v", body)
		}
		return jsonResponse(http.StatusCreated, `{"ok":true,"data":{"namespace":"user","key":"boot_count","type":"u32","value":42,"etag":"\"sha256-`+testSHA256+`\""}}`), nil
	})}
	nvs := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).NVS()
	value, err := nvs.Get(context.Background(), "user", "boot_count")
	if err != nil {
		t.Fatal(err)
	}
	if value.Type != "u32" || string(value.Value) != "41" {
		t.Errorf("value = %+v", value)
	}
	value, err = nvs.Set(context.Background(), "user", "boot_count", map[string]any{"type": "u32", "value": 42}, NVSWriteOptions{CreateOnly: true})
	if err != nil {
		t.Fatal(err)
	}
	if string(value.Value) != "42" {
		t.Errorf("value = %+v", value)
	}
}

func TestNVSDeleteClearAndBatch(t *testing.T) {
	calls := 0
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		calls++
		switch calls {
		case 1:
			if request.Method != http.MethodDelete || request.Header.Get("If-Match") != `"sha256-`+testSHA256+`"` {
				t.Errorf("delete request = %+v", request)
			}
			return jsonResponse(http.StatusNoContent, ""), nil
		case 2:
			if request.Method != http.MethodDelete || request.Header.Get("X-Confirm-Namespace") != "user_cache" {
				t.Errorf("clear request = %+v", request)
			}
			return jsonResponse(http.StatusNoContent, ""), nil
		default:
			if request.Method != http.MethodPost || request.URL.Path != "/api/v1/nvs/namespaces/user/batch" {
				t.Errorf("batch request = %+v", request)
			}
			body, _ := io.ReadAll(request.Body)
			if string(body) != `{"operations":[{"op":"delete","key":"old"}]}` {
				t.Errorf("body = %s", body)
			}
			return jsonResponse(http.StatusOK, `{"ok":true,"data":{"namespace":"user","operations":[{"op":"delete","key":"old","deleted":true}]}}`), nil
		}
	})}
	nvs := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).NVS()
	if err := nvs.Delete(context.Background(), "user", "old", testSHA256); err != nil {
		t.Fatal(err)
	}
	if err := nvs.Clear(context.Background(), "user_cache"); err != nil {
		t.Fatal(err)
	}
	result, err := nvs.Batch(context.Background(), "user", json.RawMessage(`{"operations":[{"op":"delete","key":"old"}]}`))
	if err != nil {
		t.Fatal(err)
	}
	if result.Namespace != "user" || !strings.Contains(string(result.Operations), `"deleted":true`) {
		t.Errorf("result = %+v", result)
	}
}

func TestNVSReturnsStructuredError(t *testing.T) {
	httpClient := &http.Client{Transport: roundTripFunc(func(request *http.Request) (*http.Response, error) {
		return jsonResponse(http.StatusForbidden, `{"ok":false,"error":{"code":"NVS_MANAGED","name":"ForbiddenError","message":"namespace is managed"}}`), nil
	})}
	_, err := (&Client{BaseURL: "http://jsp32.local", HTTPClient: httpClient}).NVS().Get(context.Background(), "jsp32_config", "wifi_ssid")
	httpError, ok := err.(*HTTPError)
	if !ok || httpError.Detail == nil || httpError.Detail.Code != "NVS_MANAGED" {
		t.Fatalf("error = %#v", err)
	}
}
