package command

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"strconv"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
	"tinygo.org/x/espflasher/pkg/espflasher"
)

type serialFlasher interface {
	Close() error
	FlashID() (uint8, uint16, error)
	FlashImage([]byte, uint32, espflasher.ProgressFunc) error
	ReadFlash(uint32, uint32, espflasher.ProgressFunc) ([]byte, error)
	GetFlashMD5(uint32, uint32, espflasher.ProgressFunc) (string, error)
	Reset()
}

type FlashCommand struct {
	connect func(string, *espflasher.FlasherOptions) (serialFlasher, error)
}

func (*FlashCommand) Name() string    { return "flash" }
func (*FlashCommand) Summary() string { return "flash factory or application firmware over serial" }

func (c *FlashCommand) Run(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("flash", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	port := flags.String("p", "", "serial port")
	baud := flags.Int("b", 115200, "transfer baud rate")
	alignment := flags.String("align", "64K", "flash alignment in bytes or with K/M suffix")
	reset := flags.String("reset", "default", "default, no-reset, usb-jtag, or auto")
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			c.printHelp(app.Stdout)
			return nil
		}
		return &UsageError{Message: err.Error()}
	}
	if *port == "" || flags.NArg() != 1 {
		return &UsageError{Message: "usage: jsp32 flash -p PORT [-b BAUD] [-align SIZE] [-reset MODE] firmware.bin"}
	}
	if *baud < 115200 || *baud > 921600 {
		return &UsageError{Message: "flash baud rate must be between 115200 and 921600"}
	}
	align, err := parseFlashAlignment(*alignment)
	if err != nil {
		return &UsageError{Message: err.Error()}
	}
	modes := map[string]espflasher.ResetMode{"default": espflasher.ResetDefault, "no-reset": espflasher.ResetNoReset, "usb-jtag": espflasher.ResetUSBJTAG, "auto": espflasher.ResetAuto}
	mode, ok := modes[*reset]
	if !ok {
		return &UsageError{Message: "reset mode must be default, no-reset, usb-jtag, or auto"}
	}
	data, err := readFlashFirmware(flags.Arg(0))
	if err != nil {
		return fmt.Errorf("read firmware: %w", err)
	}
	image, err := inspectFlashImage(data)
	if err != nil {
		return fmt.Errorf("inspect firmware: %w", err)
	}
	image.data = padFlashImage(image.data, align)
	if err := ctx.Err(); err != nil {
		return err
	}
	fmt.Fprintf(app.Stderr, "Detected %s firmware (%d bytes). Connecting to %s...\n", image.kind, len(data), *port)
	opts := espflasher.DefaultOptions()
	opts.ChipType, opts.ResetMode, opts.FlashBaudRate = espflasher.ChipESP32S3, mode, *baud
	// Specify the validated header value so the library does not silently patch
	// the image before our independent MD5 verification.
	opts.FlashSize = fmt.Sprintf("%dMB", image.capacity>>20)
	opts.ConnectStatus = func(_ espflasher.ConnectPhase, _, _ int, message string) { fmt.Fprintln(app.Stderr, message) }
	opts.SerialOpener = func(name string, serialMode *serial.Mode) (serial.Port, error) {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		p, err := serial.Open(name, serialMode)
		if err != nil {
			return nil, err
		}
		wrapped := &cancelSerialPort{Port: p}
		stop := context.AfterFunc(ctx, func() { _ = wrapped.Close() })
		// Stop the cancellation callback when the library releases/reopens a port.
		return &scopedSerialPort{cancelSerialPort: wrapped, stop: stop}, nil
	}
	connect := c.connect
	if connect == nil {
		connect = func(port string, opts *espflasher.FlasherOptions) (serialFlasher, error) {
			return espflasher.New(port, opts)
		}
	}
	f, err := connect(*port, opts)
	if err != nil {
		if ctx.Err() != nil {
			return ctx.Err()
		}
		return fmt.Errorf("connect: %w (without an automatic download circuit, enter BOOT/RESET manually and use -reset no-reset)", err)
	}
	defer f.Close()
	_, deviceID, err := f.FlashID()
	if err != nil {
		return fmt.Errorf("read flash capacity: %w", err)
	}
	capacityCode := uint8(deviceID)
	if capacityCode < 20 || capacityCode > 30 {
		return fmt.Errorf("unsupported JEDEC flash capacity code 0x%x", capacityCode)
	}
	if uint64(image.capacity) > uint64(1)<<capacityCode {
		return errors.New("firmware requires more flash than the connected device provides")
	}
	var otaPlan otaFlashPlan
	if image.kind == "application" {
		fmt.Fprintln(app.Stderr, "Reading device partition table...")
		table, err := f.ReadFlash(partitionTableOffset, 0xc00, nil)
		if err != nil {
			return fmt.Errorf("read device partition table: %w", err)
		}
		layout, err := parseFlashLayout(table)
		if err != nil {
			return fmt.Errorf("device partition table: %w; use a factory image for initial deployment", err)
		}
		if layout.otaData.size < 2*otaSectorSize || layout.otaData.offset%otaSectorSize != 0 {
			return errors.New("device partition table has no valid two-sector otadata partition")
		}
		fmt.Fprintln(app.Stderr, "Reading OTA boot selection...")
		otaData, err := f.ReadFlash(layout.otaData.offset, 2*otaSectorSize, nil)
		if err != nil {
			return fmt.Errorf("read otadata: %w", err)
		}
		otaPlan, err = planOTAFlash(layout, otaData)
		if err != nil {
			return err
		}
		image.offset = otaPlan.partition.offset
		fmt.Fprintf(app.Stderr, "Selected: ota_%d; target: ota_%d at 0x%x.\n", otaPlan.current, otaPlan.target, image.offset)
		if uint32(len(image.data)) > otaPlan.partition.size || layout.end > image.capacity {
			return errors.New("application does not fit the device partition layout")
		}
	}
	if image.offset%uint32(align) != 0 {
		return fmt.Errorf("flash address 0x%x is not aligned to %d bytes", image.offset, align)
	}
	if uint64(image.offset)+uint64(len(image.data)) > uint64(image.capacity) {
		return errors.New("firmware exceeds configured flash capacity")
	}
	if err := ctx.Err(); err != nil {
		return err
	}
	fmt.Fprintf(app.Stderr, "Flashing %s at 0x%x (%d bytes with %d-byte alignment)...\n", image.kind, image.offset, len(image.data), align)
	progress := &serialFlashProgress{output: app.Stderr}
	progress.update(0, len(image.data))
	err = f.FlashImage(image.data, image.offset, progress.update)
	progress.finish()
	if ctx.Err() != nil {
		return ctx.Err()
	}
	if err != nil {
		return fmt.Errorf("flash firmware: %w", err)
	}
	// The dependency treats some MD5 command errors as warnings. Require a
	// successful explicit verification before reporting completion or resetting.
	fmt.Fprintln(app.Stderr, "Verifying flash MD5...")
	if err := verifySerialFlash(ctx, f, image.data, image.offset); err != nil {
		return err
	}
	if image.kind == "application" {
		// The dependency does not update its initial baud option after changing
		// the port speed. Keep the established speed for subsequent raw writes.
		opts.FlashBaudRate = 0
		if err := activateSerialOTA(ctx, f, otaPlan, app.Stderr); err != nil {
			return err
		}
	}
	if mode == espflasher.ResetNoReset {
		fmt.Fprintln(app.Stderr, "Verification passed. Press RESET to start the firmware.")
	} else {
		f.Reset()
	}
	fmt.Fprintf(app.Stdout, "flashed\t%s\t0x%x\t%d bytes\n", image.kind, image.offset, len(data))
	return nil
}

func (*FlashCommand) printHelp(output io.Writer) {
	fmt.Fprintln(output, "Usage: jsp32 flash -p PORT [-b BAUD] [-align SIZE] [-reset MODE] firmware.bin")
	fmt.Fprintln(output, "  -p       serial port (required)")
	fmt.Fprintln(output, "  -b       transfer baud rate (default 115200)")
	fmt.Fprintln(output, "  -align   byte alignment: 4096, 0x10000, 4K, 64KiB, 128K (default 64K)")
	fmt.Fprintln(output, "  -reset   default, no-reset, usb-jtag, or auto (default default)")
	fmt.Fprintln(output, "Accepts .bin and .bin.gz; gzip firmware is decompressed before inspection.")
	fmt.Fprintln(output, "Image contents select factory (0x0) or application (inactive OTA slot).")
	fmt.Fprintln(output, "Application flashing verifies the inactive slot, then selects it for next boot.")
}

type cancelSerialPort struct {
	serial.Port
	once sync.Once
	err  error
}

func (p *cancelSerialPort) Close() error { p.once.Do(func() { p.err = p.Port.Close() }); return p.err }

type scopedSerialPort struct {
	*cancelSerialPort
	stop func() bool
}

func (p *scopedSerialPort) Close() error { p.stop(); return p.cancelSerialPort.Close() }

type serialFlashProgress struct {
	output  io.Writer
	last    time.Time
	percent int
}

func (p *serialFlashProgress) update(current, total int) {
	if total <= 0 {
		return
	}
	percent := min(100, max(0, int(float64(current)*100/float64(total))))
	now := time.Now()
	if !p.last.IsZero() && percent == p.percent && now.Sub(p.last) < time.Second {
		return
	}
	fmt.Fprintf(p.output, "\rTransfer: %3d%%  %d/%d bytes", percent, current, total)
	if percent == 100 && (p.last.IsZero() || p.percent != 100) {
		fmt.Fprint(p.output, "\nWaiting for write completion and verification...")
	}
	p.last, p.percent = now, percent
}
func (p *serialFlashProgress) finish() { fmt.Fprintln(p.output) }

func parseFlashAlignment(value string) (int, error) {
	value = strings.ToUpper(strings.TrimSpace(value))
	multiplier := uint64(1)
	for _, suffix := range []struct {
		text       string
		multiplier uint64
	}{
		{"KIB", 1024}, {"KB", 1024}, {"K", 1024},
		{"MIB", 1024 * 1024}, {"MB", 1024 * 1024}, {"M", 1024 * 1024},
	} {
		if strings.HasSuffix(value, suffix.text) {
			value = strings.TrimSuffix(value, suffix.text)
			multiplier = suffix.multiplier
			break
		}
	}
	size, err := strconv.ParseUint(value, 0, 64)
	if err != nil || size > (16<<20)/multiplier {
		return 0, errors.New("flash alignment must be a power of two from 4 KiB to 16 MiB (e.g. 4K, 64K, 128K)")
	}
	size *= multiplier
	if size < 4096 || size&(size-1) != 0 {
		return 0, errors.New("flash alignment must be a power of two from 4 KiB to 16 MiB (e.g. 4K, 64K, 128K)")
	}
	return int(size), nil
}
