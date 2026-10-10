package command

import (
	"bufio"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"strings"
)

// Image inspection currently supports Flash sizes up to 16 MiB. Bound the
// uncompressed input too, so malformed gzip data cannot exhaust memory.
const maxFlashFirmwareSize = 16 << 20

func readFlashFirmware(path string) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	buffered := bufio.NewReader(file)
	magic, err := buffered.Peek(2)
	if err != nil && err != io.EOF {
		return nil, err
	}
	var source io.Reader = buffered
	compressed := (len(magic) == 2 && magic[0] == 0x1f && magic[1] == 0x8b) || strings.HasSuffix(strings.ToLower(path), ".gz")
	if compressed {
		reader, err := gzip.NewReader(buffered)
		if err != nil {
			return nil, fmt.Errorf("open gzip firmware: %w", err)
		}
		defer reader.Close()
		source = reader
	}
	data, err := io.ReadAll(io.LimitReader(source, maxFlashFirmwareSize+1))
	if err != nil {
		if compressed {
			return nil, fmt.Errorf("decompress gzip firmware: %w", err)
		}
		return nil, err
	}
	if len(data) > maxFlashFirmwareSize {
		return nil, fmt.Errorf("uncompressed firmware exceeds %d MiB", maxFlashFirmwareSize>>20)
	}
	return data, nil
}
