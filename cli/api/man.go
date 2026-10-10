package api

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"regexp"
)

var manualTopic = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]*(\.[a-z0-9][a-z0-9_-]*)*$`)

// ValidManualTopic reports whether topic uses the dotted manual namespace.
func ValidManualTopic(topic string) bool { return manualTopic.MatchString(topic) }

// Manual returns the device's original Markdown page. Empty topic selects the index.
func (c *Client) Manual(ctx context.Context, topic string) (string, error) {
	if topic == "" {
		topic = "index"
	}
	if !ValidManualTopic(topic) {
		return "", fmt.Errorf("invalid manual topic %q", topic)
	}
	endpoint, err := c.endpoint("man/" + topic)
	if err != nil {
		return "", err
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return "", fmt.Errorf("create manual request: %w", err)
	}
	request.Header.Set("Accept", "text/markdown")
	response, err := c.httpClient().Do(request)
	if err != nil {
		return "", fmt.Errorf("query manual: %w", err)
	}
	defer response.Body.Close()
	if response.StatusCode == http.StatusNotFound {
		return "", fmt.Errorf("manual topic %q not found", topic)
	}
	if response.StatusCode != http.StatusOK {
		return "", fmt.Errorf("query manual: %s", response.Status)
	}
	body, err := io.ReadAll(response.Body)
	if err != nil {
		return "", fmt.Errorf("read manual: %w", err)
	}
	return string(body), nil
}
