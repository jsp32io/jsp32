package command

import (
	"bytes"
	"compress/gzip"
	"context"
	"crypto/md5"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"tinygo.org/x/espflasher/pkg/espflasher"
)

func testESPImage(app bool) []byte {
	data := make([]byte, 32+256)
	data[0], data[1], data[3], data[12], data[23] = 0xe9, 1, 0x40, 9, 1
	binary.LittleEndian.PutUint32(data[28:32], 256)
	if app {
		binary.LittleEndian.PutUint32(data[32:36], 0xabcd5432)
	}
	checksum := byte(0xef)
	for _, b := range data[32:] {
		checksum ^= b
	}
	data = append(data, make([]byte, 16-len(data)%16)...)
	data[len(data)-1] = checksum
	hash := sha256.Sum256(data)
	return append(data, hash[:]...)
}

func testFlashTable() []byte {
	table := bytes.Repeat([]byte{0xff}, 0xc00)
	entries := []struct {
		kind, subtype byte
		offset, size  uint32
		name          string
	}{
		{1, 0, 0x9000, 0x2000, "otadata"},
		{0, 0x10, 0x20000, 0x300000, "ota_0"},
		{0, 0x11, 0x320000, 0x300000, "ota_1"},
		{1, 0x83, 0x620000, 0x9e0000, "storage"},
	}
	for i, part := range entries {
		e := table[i*32 : (i+1)*32]
		clear(e)
		binary.LittleEndian.PutUint16(e, 0x50aa)
		e[2], e[3] = part.kind, part.subtype
		binary.LittleEndian.PutUint32(e[4:8], part.offset)
		binary.LittleEndian.PutUint32(e[8:12], part.size)
		copy(e[12:28], part.name)
	}
	pos := len(entries) * 32
	digest := md5.Sum(table[:pos])
	table[pos], table[pos+1] = 0xeb, 0xeb
	copy(table[pos+16:pos+32], digest[:])
	return table
}

func testFactoryImage() []byte {
	data := bytes.Repeat([]byte{0xff}, 0x630000)
	copy(data, testESPImage(false))
	copy(data[0x8000:], testFlashTable())
	copy(data[0x20000:], testESPImage(true))
	data[0x620000] = 0x12
	return data
}

func TestInspectFlashImage(t *testing.T) {
	app, factory := testESPImage(true), testFactoryImage()
	badChecksum := append([]byte(nil), app...)
	badChecksum[40] ^= 1
	badDigest := append([]byte(nil), app...)
	badDigest[len(badDigest)-1] ^= 1
	wrongChip := append([]byte(nil), app...)
	wrongChip[12] = 0
	badTable := append([]byte(nil), factory...)
	badTable[0x8004] ^= 1
	tests := []struct {
		name string
		data []byte
		kind string
		bad  bool
	}{
		{"application", app, "application", false},
		{"factory", factory, "factory", false},
		{"empty", nil, "", true},
		{"truncated app", app[:40], "", true},
		{"checksum", badChecksum, "", true},
		{"digest", badDigest, "", true},
		{"wrong chip", wrongChip, "", true},
		{"bootloader alone", testESPImage(false), "", true},
		{"table digest", badTable, "", true},
		{"missing storage", factory[:0x620000], "", true},
		{"trailing data", append(append([]byte(nil), app...), 0x12), "", true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			image, err := inspectFlashImage(tt.data)
			if (err != nil) != tt.bad {
				t.Fatalf("error = %v", err)
			}
			if !tt.bad && image.kind != tt.kind {
				t.Fatalf("kind = %q", image.kind)
			}
		})
	}
}

type fakeSerialFlasher struct {
	table, data                    []byte
	offset                         uint32
	flashErr, verifyErr            error
	mismatch                       bool
	closed, reset                  bool
	otaData                        []byte
	updates                        []otaBootUpdate
	lastData                       []byte
	lastOffset                     uint32
	metadataErr, metadataVerifyErr error
}

func (f *fakeSerialFlasher) FlashID() (uint8, uint16, error) { return 1, 0x2018, nil }
func (f *fakeSerialFlasher) Close() error                    { f.closed = true; return nil }
func (f *fakeSerialFlasher) Reset()                          { f.reset = true }
func (f *fakeSerialFlasher) ReadFlash(offset, size uint32, _ espflasher.ProgressFunc) ([]byte, error) {
	if offset == 0x8000 && size == 0xc00 {
		return f.table, nil
	}
	if offset == 0x9000 && size == 2*otaSectorSize {
		if f.otaData != nil {
			return f.otaData, nil
		}
		data := bytes.Repeat([]byte{0xff}, 2*otaSectorSize)
		copy(data, makeOTASector(1, otaStateValid))
		return data, nil
	}
	return nil, errors.New("unexpected read")
}
func (f *fakeSerialFlasher) FlashImage(data []byte, offset uint32, progress espflasher.ProgressFunc) error {
	f.lastData, f.lastOffset = data, offset
	if offset == 0x9000 || offset == 0xa000 {
		f.updates = append(f.updates, otaBootUpdate{offset, data})
		return f.metadataErr
	}
	f.data, f.offset = data, offset
	if progress != nil {
		progress(len(data)/2, len(data))
		progress(len(data), len(data))
	}
	return f.flashErr
}
func (f *fakeSerialFlasher) GetFlashMD5(offset, size uint32, _ espflasher.ProgressFunc) (string, error) {
	if offset != f.lastOffset || int(size) != len(f.lastData) {
		return "", errors.New("unexpected verification range")
	}
	if offset == 0x9000 || offset == 0xa000 {
		return fmtMD5(f.lastData), f.metadataVerifyErr
	}
	if f.mismatch {
		return "bad", nil
	}
	return fmtMD5(f.lastData), f.verifyErr
}
func fmtMD5(data []byte) string {
	h := md5.Sum(data)
	const hex = "0123456789abcdef"
	out := make([]byte, 32)
	for i, b := range h {
		out[i*2], out[i*2+1] = hex[b>>4], hex[b&15]
	}
	return string(out)
}

func TestFlashCommand(t *testing.T) {
	tests := []struct {
		name                                         string
		factory, manual, mismatch, blank, compressed bool
		flashErr, verifyErr                          error
	}{
		{name: "application named factory.bin"},
		{name: "factory named jsp32.bin", factory: true},
		{name: "manual reset", manual: true},
		{name: "gzip application", compressed: true},
		{name: "gzip factory", factory: true, compressed: true},
		{name: "MD5 mismatch", mismatch: true},
		{name: "MD5 failure", verifyErr: errors.New("timeout")},
		{name: "transfer failure", flashErr: errors.New("disconnect")},
		{name: "blank device", blank: true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			data := testESPImage(true)
			filename := "factory.bin"
			if tt.factory {
				data = testFactoryImage()
				filename = "jsp32.bin"
			}
			input := data
			if tt.compressed {
				filename += ".gz"
				input = testGzipFirmware(t, data)
			}
			path := filepath.Join(t.TempDir(), filename)
			if err := os.WriteFile(path, input, 0600); err != nil {
				t.Fatal(err)
			}
			f := &fakeSerialFlasher{table: testFlashTable(), mismatch: tt.mismatch, flashErr: tt.flashErr, verifyErr: tt.verifyErr}
			if tt.blank {
				f.table = bytes.Repeat([]byte{0xff}, 0xc00)
			}
			stdout, stderr := &bytes.Buffer{}, &bytes.Buffer{}
			cmd := &FlashCommand{connect: func(port string, opts *espflasher.FlasherOptions) (serialFlasher, error) {
				if port != "/dev/test" || opts.ChipType != espflasher.ChipESP32S3 || opts.FlashBaudRate != 115200 {
					t.Fatal("incorrect connection options")
				}
				return f, nil
			}}
			app := &App{Stdout: stdout, Stderr: stderr, Commands: []Command{cmd}}
			args := []string{"flash", "-p", "/dev/test"}
			if tt.manual {
				args = append(args, "-reset", "no-reset")
			}
			args = append(args, path)
			err := app.Run(context.Background(), args)
			bad := tt.mismatch || tt.blank || tt.flashErr != nil || tt.verifyErr != nil
			if (err != nil) != bad {
				t.Fatalf("error=%v", err)
			}
			if !f.closed {
				t.Fatal("port not closed")
			}
			if bad {
				if f.reset || stdout.Len() != 0 || len(f.updates) != 0 {
					t.Fatal("reported success, reset or switched boot after failure")
				}
				return
			}
			wantOffset := uint32(0x320000)
			if tt.factory {
				wantOffset = 0
			}
			if f.offset != wantOffset || len(f.data)%65536 != 0 {
				t.Fatalf("offset=%x length=%d", f.offset, len(f.data))
			}
			if !bytes.Equal(f.data[:len(data)], data) {
				t.Fatal("firmware changed")
			}
			if f.reset == tt.manual {
				t.Fatal("incorrect final reset")
			}
			for _, text := range []string{"50%", "100%", "Verifying flash MD5"} {
				if !strings.Contains(stderr.String(), text) {
					t.Errorf("progress missing %q: %s", text, stderr)
				}
			}
			if !strings.Contains(stdout.String(), "flashed") {
				t.Fatal("missing success")
			}
		})
	}
}

func TestFlashRejectsBadArgumentsAndImagesBeforeConnecting(t *testing.T) {
	path := filepath.Join(t.TempDir(), "bad.bin")
	if err := os.WriteFile(path, []byte("bad"), 0600); err != nil {
		t.Fatal(err)
	}
	cmd := &FlashCommand{connect: func(string, *espflasher.FlasherOptions) (serialFlasher, error) {
		t.Fatal("must not connect")
		return nil, nil
	}}
	app := &App{Stdout: &bytes.Buffer{}, Stderr: &bytes.Buffer{}, Commands: []Command{cmd}}
	for _, args := range [][]string{{"flash"}, {"flash", "-p", "test"}, {"flash", "-p", "test", "-reset", "invalid", path}, {"flash", "-p", "test", "-b", "0", path}, {"flash", "-p", "test", path}} {
		if err := app.Run(context.Background(), args); err == nil {
			t.Errorf("accepted %v", args)
		}
	}
	if err := app.Run(context.Background(), []string{"flash", "--help"}); err != nil {
		t.Fatal(err)
	}
}

func TestRealBuildFlashImages(t *testing.T) {
	for _, name := range []string{"jsp32.bin", "factory.bin"} {
		path := filepath.Join("..", "..", "build", name)
		data, err := os.ReadFile(path)
		if errors.Is(err, os.ErrNotExist) {
			continue
		}
		if err != nil {
			t.Fatal(err)
		}
		if _, err = inspectFlashImage(data); err != nil {
			t.Fatalf("%s: %v", name, err)
		}
	}
}

func TestFlashAlignment(t *testing.T) {
	for _, tt := range []struct {
		value string
		want  int
	}{
		{"4096", 4096}, {"0x10000", 65536}, {"4K", 4096}, {"64KiB", 65536}, {"128k", 131072}, {"1M", 1048576}, {"16MiB", 16 << 20},
	} {
		got, err := parseFlashAlignment(tt.value)
		if err != nil || got != tt.want {
			t.Errorf("%q: got %d, %v", tt.value, got, err)
		}
	}
	for _, value := range []string{"", "0", "-1", "1K", "12K", "32M", "18446744073709551615M", "abc"} {
		if _, err := parseFlashAlignment(value); err == nil {
			t.Errorf("accepted %q", value)
		}
	}
	for _, alignment := range []int{4096, 65536, 131072} {
		data := bytes.Repeat([]byte{0x12}, 4097)
		padded := padFlashImage(data, alignment)
		if len(padded)%alignment != 0 || !bytes.Equal(data, padded[:len(data)]) || len(bytes.Trim(padded[len(data):], "\xff")) != 0 {
			t.Fatalf("bad padding for %d", alignment)
		}
	}
}

func TestFlashCustomAlignment(t *testing.T) {
	for _, tt := range []struct {
		value string
		size  int
		fail  bool
	}{
		{"4K", 4096, false}, {"128K", 131072, false}, {"256K", 0, true},
	} {
		t.Run(tt.value, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "app.bin")
			if err := os.WriteFile(path, testESPImage(true), 0600); err != nil {
				t.Fatal(err)
			}
			f := &fakeSerialFlasher{table: testFlashTable()}
			cmd := &FlashCommand{connect: func(string, *espflasher.FlasherOptions) (serialFlasher, error) { return f, nil }}
			out := &bytes.Buffer{}
			app := &App{Stdout: out, Stderr: &bytes.Buffer{}, Commands: []Command{cmd}}
			err := app.Run(context.Background(), []string{"flash", "-p", "test", "-align", tt.value, path})
			if (err != nil) != tt.fail {
				t.Fatalf("error=%v", err)
			}
			if tt.fail {
				if f.data != nil || f.reset || out.Len() != 0 {
					t.Fatal("wrote unaligned image")
				}
				return
			}
			if len(f.data) != tt.size {
				t.Fatalf("size=%d", len(f.data))
			}
		})
	}
}

func testGzipFirmware(t *testing.T, data []byte) []byte {
	t.Helper()
	var buf bytes.Buffer
	writer := gzip.NewWriter(&buf)
	if _, err := writer.Write(data); err != nil {
		t.Fatal(err)
	}
	if err := writer.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

func TestReadFlashFirmware(t *testing.T) {
	data := testESPImage(true)
	compressed := testGzipFirmware(t, data)
	badCRC := append([]byte(nil), compressed...)
	badCRC[len(badCRC)-8] ^= 1
	oversized := testGzipFirmware(t, make([]byte, maxFlashFirmwareSize+1))
	for _, tt := range []struct {
		name  string
		input []byte
		fail  bool
	}{
		{"raw.bin", data, false},
		{"app.bin.gz", compressed, false},
		{"renamed.bin", compressed, false},
		{"uppercase.bin.GZ", compressed, false},
		{"invalid.bin.gz", data, true},
		{"truncated.bin.gz", compressed[:len(compressed)-1], true},
		{"checksum.bin.gz", badCRC, true},
		{"too-large.bin.gz", oversized, true},
	} {
		t.Run(tt.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), tt.name)
			if err := os.WriteFile(path, tt.input, 0600); err != nil {
				t.Fatal(err)
			}
			got, err := readFlashFirmware(path)
			if (err != nil) != tt.fail {
				t.Fatalf("error=%v", err)
			}
			if !tt.fail && !bytes.Equal(got, data) {
				t.Fatal("firmware contents changed")
			}
		})
	}
}

func TestFlashRejectsDamagedGzipBeforeConnecting(t *testing.T) {
	compressed := testGzipFirmware(t, testESPImage(true))
	compressed[len(compressed)-8] ^= 1
	path := filepath.Join(t.TempDir(), "app.bin.gz")
	if err := os.WriteFile(path, compressed, 0600); err != nil {
		t.Fatal(err)
	}
	cmd := &FlashCommand{connect: func(string, *espflasher.FlasherOptions) (serialFlasher, error) {
		t.Fatal("connected before gzip verification")
		return nil, nil
	}}
	app := &App{Stdout: &bytes.Buffer{}, Stderr: &bytes.Buffer{}, Commands: []Command{cmd}}
	if err := app.Run(context.Background(), []string{"flash", "-p", "test", path}); err == nil {
		t.Fatal("accepted corrupt gzip")
	}
}
