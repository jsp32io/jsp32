package command

import (
	"bytes"
	"context"
	"crypto/md5"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
)

const otaSectorSize = 4096
const otaStateValid = 2

type otaRecord struct {
	seq, state uint32
	valid      bool
}
type otaBootUpdate struct {
	offset uint32
	data   []byte
}
type otaFlashPlan struct {
	current, target int
	partition       flashPartition
	updates         []otaBootUpdate
}

// ESP-IDF uses esp_rom_crc32_le(UINT32_MAX, little-endian ota_seq, 4).
func otaSequenceCRC(seq uint32) uint32 {
	var raw [4]byte
	binary.LittleEndian.PutUint32(raw[:], seq)
	return crc32.Update(^uint32(0), crc32.IEEETable, raw[:])
}

func decodeOTARecord(data []byte) otaRecord {
	seq := binary.LittleEndian.Uint32(data[:4])
	state := binary.LittleEndian.Uint32(data[24:28])
	crc := binary.LittleEndian.Uint32(data[28:32])
	return otaRecord{seq, state, seq != 0 && seq != ^uint32(0) && state != 3 && state != 4 && crc == otaSequenceCRC(seq)}
}

func makeOTASector(seq, state uint32) []byte {
	data := bytes.Repeat([]byte{0xff}, otaSectorSize)
	binary.LittleEndian.PutUint32(data[:4], seq)
	binary.LittleEndian.PutUint32(data[24:28], state)
	binary.LittleEndian.PutUint32(data[28:32], otaSequenceCRC(seq))
	return data
}

func planOTAFlash(layout flashLayout, data []byte) (otaFlashPlan, error) {
	var plan otaFlashPlan
	if layout.otaCount != 2 || layout.apps[0].size == 0 || layout.apps[1].size == 0 || layout.hasFactory {
		return plan, errors.New("serial application updates require exactly ota_0 and ota_1, without a factory application partition")
	}
	if layout.encrypted {
		return plan, errors.New("encrypted OTA partitions are not supported by serial flash")
	}
	if layout.otaData.offset%otaSectorSize != 0 || layout.otaData.size < 2*otaSectorSize || len(data) != 2*otaSectorSize {
		return plan, errors.New("otadata must contain two independently erasable 4 KiB sectors")
	}
	records := [2]otaRecord{decodeOTARecord(data[:32]), decodeOTARecord(data[otaSectorSize : otaSectorSize+32])}
	active := -1
	for i, r := range records {
		// On equal sequences ESP-IDF selects sector zero.
		if r.valid && (active < 0 || r.seq > records[active].seq) {
			active = i
		}
	}
	if active < 0 {
		if !bytes.Equal(data[:32], bytes.Repeat([]byte{0xff}, 32)) || !bytes.Equal(data[otaSectorSize:otaSectorSize+32], bytes.Repeat([]byte{0xff}, 32)) {
			return plan, errors.New("no valid OTA boot record; refusing to guess the active slot (use a factory image)")
		}
		// Erased otadata selects ota_0 on this layout. Seed a confirmed fallback
		// record after image verification, then activate ota_1 in the other sector.
		plan.current, plan.target = 0, 1
		plan.updates = []otaBootUpdate{
			{layout.otaData.offset, makeOTASector(1, otaStateValid)},
			{layout.otaData.offset + otaSectorSize, makeOTASector(2, otaStateValid)},
		}
	} else {
		r := records[active]
		plan.current = int((r.seq - 1) % 2)
		plan.target = 1 - plan.current
		nextSeq := uint64(r.seq) + 1
		// FlashImage treats a leading 0xe9 as an ESP image and patches its
		// header. Skip that sequence (by two to keep the same OTA slot)
		// so raw boot metadata can never be mistaken for an image.
		if byte(nextSeq) == 0xe9 {
			nextSeq += 2
		}
		if nextSeq >= uint64(^uint32(0)) {
			return plan, errors.New("OTA sequence exhausted; use a factory image")
		}
		plan.updates = []otaBootUpdate{{layout.otaData.offset + uint32(1-active)*otaSectorSize, makeOTASector(uint32(nextSeq), otaStateValid)}}
	}
	plan.partition = layout.apps[plan.target]
	return plan, nil
}

func verifySerialFlash(ctx context.Context, f serialFlasher, data []byte, offset uint32) error {
	if err := ctx.Err(); err != nil {
		return err
	}
	actual, err := f.GetFlashMD5(offset, uint32(len(data)), nil)
	if ctx.Err() != nil {
		return ctx.Err()
	}
	if err != nil {
		return fmt.Errorf("verify flash at 0x%x: %w", offset, err)
	}
	expected := fmt.Sprintf("%x", md5.Sum(data))
	if actual != expected {
		return fmt.Errorf("flash MD5 mismatch at 0x%x: expected %s, got %s", offset, expected, actual)
	}
	return nil
}

func activateSerialOTA(ctx context.Context, f serialFlasher, plan otaFlashPlan, output io.Writer) error {
	fmt.Fprintf(output, "Setting next boot to ota_%d...\n", plan.target)
	for _, update := range plan.updates {
		if err := ctx.Err(); err != nil {
			return err
		}
		// OTA metadata always uses 4 KiB sectors, independently of application
		// padding. Never pad this write to -align: it would erase adjacent data.
		if err := f.FlashImage(update.data, update.offset, nil); err != nil {
			return fmt.Errorf("write OTA boot record: %w", err)
		}
		if err := verifySerialFlash(ctx, f, update.data, update.offset); err != nil {
			return fmt.Errorf("verify OTA boot record: %w", err)
		}
	}
	fmt.Fprintf(output, "Next boot: ota_%d.\n", plan.target)
	return nil
}
