package command

import (
	"context"
	"fmt"
	"io"
	"strings"
	"time"

	"github.com/jsp32io/jsp32/cli/api"
)

const otaProgressInterval = 500 * time.Millisecond

// Only this goroutine renders progress. The upload cancels an in-flight status
// probe on completion, so an unresponsive probe cannot delay the upload result.
func uploadWithDeviceProgress(ctx context.Context, ota *api.OTA, firmware io.Reader,
	size int64, checksum, id string, output io.Writer, interval time.Duration) (*api.OTAUploadResult, error) {
	type outcome struct {
		result *api.OTAUploadResult
		err    error
	}
	done := make(chan outcome, 1)
	progressCtx, stopProgress := context.WithCancel(ctx)
	defer stopProgress()
	go func() {
		result, err := ota.UploadWithID(ctx, firmware, size, checksum, id)
		done <- outcome{result, err}
		stopProgress()
	}()
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	defer fmt.Fprintln(output)
	var lastWritten int64 = -1
	var lastState, previousLine string
	render := func(line string) {
		if line == previousLine {
			return
		}
		// Status text is ASCII. Backspace to its start, replace it, then erase
		// any old suffix and leave the cursor immediately after the new text.
		update := strings.Repeat("\b", len(previousLine)) + line
		if extra := len(previousLine) - len(line); extra > 0 {
			update += strings.Repeat(" ", extra) + strings.Repeat("\b", extra)
		}
		fmt.Fprint(output, update)
		previousLine = line
	}
	render("waiting for device...")
	for {
		select {
		case completed := <-done:
			if completed.err != nil {
				return nil, completed.err
			}
			if completed.result.State != "ready" {
				return nil, fmt.Errorf("unexpected OTA upload state %q", completed.result.State)
			}
			render("ready  100.0%")
			return completed.result, nil
		case <-ticker.C:
			probeCtx, cancel := context.WithTimeout(progressCtx, 2*time.Second)
			status, err := ota.Status(probeCtx)
			cancel()
			if err != nil {
				if progressCtx.Err() == nil {
					render("uploading (progress unavailable)")
				}
				lastState = ""
				continue
			}
			p := status.Upload
			if p != nil && p.Written == nil {
				render("uploading (progress unsupported)")
				lastState = ""
				continue
			}
			if p == nil || p.ID != id || p.Total != size {
				render("waiting for device...")
				lastState = ""
				continue
			}
			written := *p.Written
			if written < 0 || written > size || written < lastWritten {
				continue
			}
			if written == lastWritten && status.State == lastState {
				continue
			}
			if status.State != "receiving" && status.State != "verifying" {
				continue
			}
			lastWritten, lastState = written, status.State
			if written == size || status.State == "verifying" {
				render("verifying...")
			} else {
				percent := float64(written) * 100 / float64(size)
				if percent > 99.9 {
					percent = 99.9
				}
				render(fmt.Sprintf("writing %6.1f%%  %d/%d bytes", percent, written, size))
			}
		}
	}
}
