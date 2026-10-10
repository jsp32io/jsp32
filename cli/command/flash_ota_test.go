package command

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"tinygo.org/x/espflasher/pkg/espflasher"
)

func testOTAData(seq0, state0, seq1, state1 uint32) []byte {
	data := bytes.Repeat([]byte{0xff}, 2*otaSectorSize)
	if seq0 != ^uint32(0) {
		copy(data, makeOTASector(seq0, state0))
	}
	if seq1 != ^uint32(0) {
		copy(data[otaSectorSize:], makeOTASector(seq1, state1))
	}
	return data
}

func TestOTASequenceCRCMatchesESPROM(t *testing.T) {
	// Independent golden values from ESP ROM's reflected CRC32, initial ~0.
	for seq, want := range map[uint32]uint32{1: 0x4743989a, 2: 0x55f63774, 3: 0xed4a5011, 4: 0x709d68a8} {
		if got := otaSequenceCRC(seq); got != want {
			t.Fatalf("seq %d: CRC %x, want %x", seq, got, want)
		}
	}
}

func TestPlanOTAFlash(t *testing.T) {
	layout, err := parseFlashLayout(testFlashTable())
	if err != nil {
		t.Fatal(err)
	}
	erased := ^uint32(0)
	tests := []struct {
		name            string
		data            []byte
		current, target int
		offset, seq     uint32
		count           int
		fail            bool
	}{
		{"ota0 selected", testOTAData(3, 2, 2, 2), 0, 1, 0xa000, 4, 1, false},
		{"ota1 selected", testOTAData(1, 2, 2, 2), 1, 0, 0x9000, 3, 1, false},
		{"equal selects first record", testOTAData(4, 2, 4, 2), 1, 0, 0xa000, 5, 1, false},
		{"invalid newest ignored", testOTAData(3, 2, 4, 3), 0, 1, 0xa000, 4, 1, false},
		{"aborted newest ignored", testOTAData(3, 2, 4, 4), 0, 1, 0xa000, 4, 1, false},
		{"erased fallback", testOTAData(erased, erased, erased, erased), 0, 1, 0xa000, 2, 2, false},
		{"pending selected firmware", testOTAData(1, 2, 2, 1), 1, 0, 0x9000, 3, 1, false},
		{"both invalid rejected", testOTAData(3, 3, 4, 4), 0, 0, 0, 0, 0, true},
		{"sequence exhausted", testOTAData(0xfffffffe, 2, erased, erased), 0, 0, 0, 0, 0, true},
		{"raw header collision skipped", testOTAData(232, 2, erased, erased), 1, 0, 0xa000, 235, 1, false},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			before := append([]byte(nil), tt.data...)
			plan, err := planOTAFlash(layout, tt.data)
			if (err != nil) != tt.fail {
				t.Fatalf("error=%v", err)
			}
			if !bytes.Equal(before, tt.data) {
				t.Fatal("mutated original metadata")
			}
			if tt.fail {
				return
			}
			if plan.current != tt.current || plan.target != tt.target || plan.partition != layout.apps[tt.target] || len(plan.updates) != tt.count {
				t.Fatalf("plan=%+v", plan)
			}
			last := plan.updates[len(plan.updates)-1]
			if last.offset != tt.offset || len(last.data) != otaSectorSize {
				t.Fatal("wrong metadata sector")
			}
			record := decodeOTARecord(last.data)
			if !record.valid || record.seq != tt.seq || record.state != otaStateValid || int((record.seq-1)%2) != tt.target {
				t.Fatalf("record=%+v", record)
			}
			if len(plan.updates) == 2 {
				fallback := decodeOTARecord(plan.updates[0].data)
				if fallback.seq != 1 || fallback.state != otaStateValid || plan.updates[0].offset != 0x9000 {
					t.Fatal("invalid fallback")
				}
			}
		})
	}
	data := testOTAData(1, 2, 2, 2)
	data[otaSectorSize+28] ^= 1
	plan, err := planOTAFlash(layout, data)
	if err != nil || plan.current != 0 || plan.target != 1 {
		t.Fatalf("bad CRC not ignored: %+v %v", plan, err)
	}
	data[28] ^= 1
	if _, err := planOTAFlash(layout, data); err == nil {
		t.Fatal("accepted corrupt metadata")
	}
	for _, mutate := range []func(*flashLayout){
		func(l *flashLayout) { l.apps[1].size = 0 },
		func(l *flashLayout) { l.otaCount = 3 },
		func(l *flashLayout) { l.hasFactory = true },
		func(l *flashLayout) { l.encrypted = true },
		func(l *flashLayout) { l.otaData.size = 4096 },
	} {
		bad := layout
		mutate(&bad)
		if _, err := planOTAFlash(bad, testOTAData(1, 2, erased, erased)); err == nil {
			t.Fatal("accepted unsupported layout")
		}
	}
	if _, err := planOTAFlash(layout, []byte{1}); err == nil {
		t.Fatal("accepted truncated metadata")
	}
}

func TestFlashApplicationAlternatesBootSlots(t *testing.T) {
	for _, tt := range []struct {
		name                   string
		metadata               []byte
		wantOffset             uint32
		writes                 int
		metadataErr, verifyErr error
		fail                   bool
	}{
		{"ota0 to ota1", testOTAData(1, 2, ^uint32(0), 0), 0x320000, 1, nil, nil, false},
		{"ota1 to ota0", testOTAData(1, 2, 2, 2), 0x20000, 1, nil, nil, false},
		{"erased to ota1", testOTAData(^uint32(0), 0, ^uint32(0), 0), 0x320000, 2, nil, nil, false},
		{"activation write failed", testOTAData(1, 2, 2, 2), 0x20000, 1, errors.New("write failed"), nil, true},
		{"activation verify failed", testOTAData(1, 2, 2, 2), 0x20000, 1, nil, errors.New("MD5 timeout"), true},
		{"pending selected firmware", testOTAData(1, 2, 2, 1), 0x20000, 1, nil, nil, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			path := filepath.Join(t.TempDir(), "app.bin")
			if err := os.WriteFile(path, testESPImage(true), 0600); err != nil {
				t.Fatal(err)
			}
			before := append([]byte(nil), tt.metadata...)
			f := &fakeSerialFlasher{table: testFlashTable(), otaData: tt.metadata, metadataErr: tt.metadataErr, metadataVerifyErr: tt.verifyErr}
			cmd := &FlashCommand{connect: func(string, *espflasher.FlasherOptions) (serialFlasher, error) { return f, nil }}
			out, progress := &bytes.Buffer{}, &bytes.Buffer{}
			app := &App{Stdout: out, Stderr: progress, Commands: []Command{cmd}}
			err := app.Run(context.Background(), []string{"flash", "-p", "test", path})
			if (err != nil) != tt.fail {
				t.Fatalf("error=%v", err)
			}
			if f.offset != tt.wantOffset || len(f.updates) != tt.writes {
				t.Fatalf("offset=%x metadata writes=%d", f.offset, len(f.updates))
			}
			if !bytes.Equal(before, tt.metadata) {
				t.Fatal("active metadata modified")
			}
			if tt.fail {
				if f.reset || out.Len() != 0 {
					t.Fatal("success after failed activation")
				}
				return
			}
			if !f.reset || !strings.Contains(progress.String(), "Next boot: ota_") {
				t.Fatal("missing activation/reset")
			}
			if strings.Contains(progress.String(), "ota confirm") {
				t.Fatal("serial flash requested OTA confirmation")
			}
			for _, update := range f.updates {
				if decodeOTARecord(update.data).state != otaStateValid {
					t.Fatal("serial flash left an unconfirmed record")
				}
			}
			// The old valid record must remain outside every metadata write.
			if tt.writes == 1 {
				active := 0
				if binary.LittleEndian.Uint32(tt.metadata[:4]) < binary.LittleEndian.Uint32(tt.metadata[otaSectorSize:otaSectorSize+4]) && binary.LittleEndian.Uint32(tt.metadata[otaSectorSize:otaSectorSize+4]) != ^uint32(0) {
					active = 1
				}
				if f.updates[0].offset == 0x9000+uint32(active)*otaSectorSize {
					t.Fatal("overwrote active boot record")
				}
			}
		})
	}
}
