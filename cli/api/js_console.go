package api

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
)

const maxConsoleEventSize = 1024 * 1024

type ConsoleEvent struct {
	Type string
	Data string
}

type ConsoleStream struct {
	body    io.ReadCloser
	scanner *bufio.Scanner
	event   string
	data    []string
}

// Console subscribes to recent and live JavaScript console events.
// The returned stream must be closed by the caller.
func (js *JS) Console(ctx context.Context) (*ConsoleStream, error) {
	if js == nil || js.client == nil {
		return nil, errors.New("api client is nil")
	}

	endpoint, err := js.client.endpoint("quickjs/console/events")
	if err != nil {
		return nil, err
	}
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, fmt.Errorf("create console request: %w", err)
	}
	request.Header.Set("Accept", "text/event-stream")

	response, err := js.client.httpClient().Do(request)
	if err != nil {
		return nil, fmt.Errorf("connect to JSP32 console: %w", err)
	}
	if response.StatusCode < http.StatusOK || response.StatusCode >= http.StatusMultipleChoices {
		response.Body.Close()
		return nil, &HTTPError{StatusCode: response.StatusCode, Status: response.Status}
	}

	return newConsoleStream(response.Body), nil
}

func newConsoleStream(body io.ReadCloser) *ConsoleStream {
	scanner := bufio.NewScanner(body)
	scanner.Buffer(make([]byte, 4096), maxConsoleEventSize)
	return &ConsoleStream{body: body, scanner: scanner}
}

// Next waits for and returns the next SSE event. Comments and heartbeats are
// consumed internally.
func (s *ConsoleStream) Next() (*ConsoleEvent, error) {
	if s == nil || s.scanner == nil {
		return nil, errors.New("console stream is nil")
	}
	for s.scanner.Scan() {
		line := s.scanner.Text()
		if line == "" {
			if len(s.data) == 0 {
				s.event = ""
				continue
			}
			eventType := s.event
			if eventType == "" {
				eventType = "message"
			}
			event := &ConsoleEvent{Type: eventType, Data: strings.Join(s.data, "\n")}
			s.event = ""
			s.data = s.data[:0]
			return event, nil
		}
		if strings.HasPrefix(line, ":") {
			continue
		}

		field, value, found := strings.Cut(line, ":")
		if !found {
			value = ""
		} else if strings.HasPrefix(value, " ") {
			value = value[1:]
		}
		switch field {
		case "event":
			s.event = value
		case "data":
			s.data = append(s.data, value)
		}
	}
	if err := s.scanner.Err(); err != nil {
		return nil, fmt.Errorf("read console stream: %w", err)
	}
	return nil, io.EOF
}

func (s *ConsoleStream) Close() error {
	if s == nil || s.body == nil {
		return nil
	}
	return s.body.Close()
}
