package api

import (
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"regexp"
	"strings"
)

var apiVersionPath = regexp.MustCompile(`/api/v[0-9]+$`)

// Client is a client for the versioned JSP32 HTTP API.
//
// BaseURL may be either a device URL, such as http://jsp32.local, or a
// versioned API URL, such as http://jsp32.local/api/v1. A missing API version
// is normalized to /api/v1 when a request is made.
type Client struct {
	BaseURL    string
	HTTPClient *http.Client
}

func (c *Client) httpClient() *http.Client {
	if c != nil && c.HTTPClient != nil {
		return c.HTTPClient
	}
	return http.DefaultClient
}

func (c *Client) endpoint(resourcePath string) (string, error) {
	if c == nil {
		return "", errors.New("api client is nil")
	}

	rawBaseURL := strings.TrimSpace(c.BaseURL)
	if rawBaseURL == "" {
		return "", errors.New("JSP32_BASE_URL is required")
	}

	baseURL, err := url.Parse(rawBaseURL)
	if err != nil {
		return "", fmt.Errorf("invalid JSP32_BASE_URL: %w", err)
	}
	if baseURL.Scheme != "http" && baseURL.Scheme != "https" {
		return "", errors.New("invalid JSP32_BASE_URL: URL must use http or https")
	}
	if baseURL.Host == "" {
		return "", errors.New("invalid JSP32_BASE_URL: URL must include a host")
	}
	if baseURL.User != nil {
		return "", errors.New("invalid JSP32_BASE_URL: credentials are not allowed")
	}
	if baseURL.RawQuery != "" || baseURL.Fragment != "" {
		return "", errors.New("invalid JSP32_BASE_URL: query and fragment are not allowed")
	}

	path := strings.TrimRight(baseURL.EscapedPath(), "/")
	if path == "" {
		path = "/api/v1"
	} else if !apiVersionPath.MatchString(path) {
		path += "/api/v1"
	}
	path += "/" + strings.TrimLeft(resourcePath, "/")

	decodedPath, err := url.PathUnescape(path)
	if err != nil {
		return "", fmt.Errorf("invalid JSP32_BASE_URL path: %w", err)
	}
	baseURL.Path = decodedPath
	baseURL.RawPath = path
	return baseURL.String(), nil
}
