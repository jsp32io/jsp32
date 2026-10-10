package command

import (
	"bytes"
	"crypto/md5"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"
)

// ESP-IDF application partitions require 64 KiB address alignment, independent
// of the selected flash erase alignment.
const appPartitionAlignment = 65536
const partitionTableOffset = 0x8000

type flashPartition struct{ offset, size uint32 }
type flashLayout struct {
	app        flashPartition
	apps       [2]flashPartition
	otaData    flashPartition
	otaCount   int
	hasFactory bool
	encrypted  bool
	end        uint32
	storage    flashPartition
}
type flashImage struct {
	kind     string
	data     []byte
	offset   uint32
	capacity uint32
}

// validateESPImage checks segments, XOR checksum and the optional image digest.
// Both the bootloader and application start with 0xe9; the application descriptor
// in the first segment distinguishes them without relying on a file name.
func validateESPImage(data []byte, application bool) (int, error) {
	if len(data) < 32 || data[0] != 0xe9 || data[1] == 0 || data[1] > 16 {
		return 0, errors.New("invalid ESP image header")
	}
	if binary.LittleEndian.Uint16(data[12:14]) != 9 {
		return 0, errors.New("firmware is not for ESP32-S3")
	}
	if data[23] > 1 {
		return 0, errors.New("invalid image digest flag")
	}
	pos := 24
	checksum := byte(0xef)
	for i := 0; i < int(data[1]); i++ {
		if pos+8 > len(data) {
			return 0, errors.New("truncated segment header")
		}
		size := uint64(binary.LittleEndian.Uint32(data[pos+4 : pos+8]))
		pos += 8
		if size > uint64(len(data)-pos) {
			return 0, errors.New("truncated segment data")
		}
		if i == 0 {
			isApp := size >= 256 && binary.LittleEndian.Uint32(data[pos:pos+4]) == 0xabcd5432
			if isApp != application {
				return 0, errors.New("unexpected bootloader/application image type")
			}
		}
		for _, b := range data[pos : pos+int(size)] {
			checksum ^= b
		}
		pos += int(size)
	}
	end := pos + 16 - pos%16
	if end > len(data) {
		return 0, errors.New("truncated image checksum")
	}
	if data[end-1] != checksum {
		return 0, errors.New("image checksum mismatch")
	}
	if data[23] == 1 {
		if end+32 > len(data) {
			return 0, errors.New("truncated image digest")
		}
		digest := sha256.Sum256(data[:end])
		if !bytes.Equal(digest[:], data[end:end+32]) {
			return 0, errors.New("image SHA-256 mismatch")
		}
		end += 32
	}
	return end, nil
}

func parseFlashLayout(table []byte) (flashLayout, error) {
	var layout flashLayout
	var parts []flashPartition
	foundApp := false
	for pos := 0; pos+32 <= len(table); pos += 32 {
		entry := table[pos : pos+32]
		magic := binary.LittleEndian.Uint16(entry[:2])
		if magic == 0xebeb {
			digest := md5.Sum(table[:pos])
			if !bytes.Equal(digest[:], entry[16:32]) {
				return layout, errors.New("partition table MD5 mismatch")
			}
			if !foundApp {
				return layout, errors.New("partition table has no ota_0 application")
			}
			return layout, nil
		}
		if magic != 0x50aa {
			return layout, errors.New("invalid or incomplete partition table")
		}
		part := flashPartition{binary.LittleEndian.Uint32(entry[4:8]), binary.LittleEndian.Uint32(entry[8:12])}
		if part.size == 0 || part.offset < 0x9000 || uint64(part.offset)+uint64(part.size) > 16<<20 {
			return layout, errors.New("invalid partition range")
		}
		for _, other := range parts {
			if part.offset < other.offset+other.size && other.offset < part.offset+part.size {
				return layout, errors.New("overlapping partitions")
			}
		}
		parts = append(parts, part)
		if end := part.offset + part.size; end > layout.end {
			layout.end = end
		}
		if entry[2] == 0 && entry[3] >= 0x10 && entry[3] <= 0x1f {
			layout.otaCount++
			if entry[3] <= 0x11 {
				idx := int(entry[3] - 0x10)
				if layout.apps[idx].size != 0 || part.offset%appPartitionAlignment != 0 {
					return layout, errors.New("invalid or duplicate OTA application partition")
				}
				layout.apps[idx] = part
			}
		}
		if entry[2] == 0 && entry[3] == 0 {
			layout.hasFactory = true
		}
		if entry[2] == 1 && entry[3] == 0 {
			if layout.otaData.size != 0 {
				return layout, errors.New("duplicate otadata partition")
			}
			layout.otaData = part
		}
		if (entry[2] == 0 || (entry[2] == 1 && entry[3] == 0)) && binary.LittleEndian.Uint32(entry[28:32])&1 != 0 {
			layout.encrypted = true
		}
		if entry[2] == 0 && entry[3] == 0x10 {
			if foundApp || part.offset%appPartitionAlignment != 0 {
				return layout, errors.New("invalid ota_0 partition")
			}
			layout.app, foundApp = part, true
		}
		if entry[2] == 1 && string(bytes.TrimRight(entry[12:28], "\x00")) == "storage" {
			layout.storage = part
		}
	}
	return layout, errors.New("partition table is missing its MD5 record")
}

func inspectFlashImage(data []byte) (flashImage, error) {
	var image flashImage
	if len(data) < 36 || data[0] != 0xe9 {
		return image, errors.New("expected an ESP32-S3 application or merged factory binary")
	}
	sizeCode := data[3] >> 4
	if sizeCode > 4 {
		return image, errors.New("unsupported flash size in image header (maximum 16 MB)")
	}
	image.capacity = uint32(1) << (20 + sizeCode)
	isApp := binary.LittleEndian.Uint32(data[32:36]) == 0xabcd5432
	end, err := validateESPImage(data, isApp)
	if err != nil {
		return image, err
	}
	image.data = data
	if isApp {
		// Reject trailing unrelated data; esptool images may have FF alignment padding.
		if len(bytes.Trim(data[end:], "\xff")) != 0 {
			return image, errors.New("unexpected data after application image")
		}
		image.kind, image.offset = "application", 0x20000
		return image, nil
	}
	if end > partitionTableOffset || len(data) < partitionTableOffset+0xc00 {
		return image, errors.New("factory image is missing its partition table")
	}
	layout, err := parseFlashLayout(data[partitionTableOffset : partitionTableOffset+0xc00])
	if err != nil {
		return image, fmt.Errorf("factory partition table: %w", err)
	}
	if layout.end > image.capacity || uint64(len(data)) > uint64(layout.end) {
		return image, errors.New("factory image exceeds its flash/partition layout")
	}
	if layout.storage.size == 0 || uint64(len(data)) <= uint64(layout.storage.offset) {
		return image, errors.New("factory image is missing its initial storage image")
	}
	if uint64(layout.app.offset)+32 > uint64(len(data)) {
		return image, errors.New("factory image is missing its application")
	}
	appEnd, err := validateESPImage(data[layout.app.offset:], true)
	if err != nil {
		return image, fmt.Errorf("factory application: %w", err)
	}
	if uint32(appEnd) > layout.app.size {
		return image, errors.New("factory application exceeds ota_0 partition")
	}
	image.kind = "factory"
	return image, nil
}

func padFlashImage(data []byte, alignment int) []byte {
	size := (len(data) + alignment - 1) / alignment * alignment
	padded := make([]byte, size)
	copy(padded, data)
	for i := len(data); i < size; i++ {
		padded[i] = 0xff
	}
	return padded
}
