package command

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"errors"
	"flag"
	"fmt"
	"io"

	"github.com/jsp32io/jsp32/cli/api"
)

type OTACommand struct{}

func (c *OTACommand) Name() string { return "ota" }

func (c *OTACommand) Summary() string { return "update and roll back device firmware" }

func (c *OTACommand) Run(ctx context.Context, app *App, args []string) error {
	if len(args) == 0 || args[0] == "help" || args[0] == "-h" || args[0] == "--help" {
		c.printHelp(app.Stdout)
		return nil
	}
	switch args[0] {
	case "status":
		return c.status(ctx, app, args[1:])
	case "upload":
		return c.upload(ctx, app, args[1:])
	case "confirm":
		return c.confirm(ctx, app, args[1:])
	case "rollback":
		return c.rollback(ctx, app, args[1:])
	default:
		return &UsageError{Message: fmt.Sprintf("unknown ota command %q", args[0])}
	}
}

func (c *OTACommand) status(ctx context.Context, app *App, args []string) error {
	if len(args) != 0 {
		return &UsageError{Message: "ota status does not accept arguments"}
	}
	status, err := app.API.OTA().Status(ctx)
	if err != nil {
		return err
	}
	fmt.Fprintf(app.Stdout, "state\t%s\n", status.State)
	printOTAPartition(app.Stdout, "running", status.Running)
	printOTAPartition(app.Stdout, "boot", status.Boot)
	printOTAPartition(app.Stdout, "next", status.Next)
	fmt.Fprintf(app.Stdout, "rollback-enabled\t%t\nrollback-possible\t%t\n", status.Rollback.Enabled, status.Rollback.Possible)
	if status.Upload != nil {
		fmt.Fprintf(app.Stdout, "received\t%d/%d\t%.1f%%\n", status.Upload.Received, status.Upload.Total, status.Upload.Percent)
		if status.Upload.Written != nil {
			fmt.Fprintf(app.Stdout, "written\t%d/%d\t%.1f%%\n", *status.Upload.Written, status.Upload.Total, status.Upload.WrittenPercent)
		}
	}
	if status.LastError != nil {
		fmt.Fprintf(app.Stdout, "last-error\t%s\n", *status.LastError)
	}
	return nil
}

func printOTAPartition(output io.Writer, label string, partition *api.OTAPartition) {
	if partition == nil {
		fmt.Fprintf(output, "%s\t-\n", label)
		return
	}
	fmt.Fprintf(output, "%s\t%s\t0x%x\t%d\t%s\t%s\n", label, partition.Partition, partition.Address, partition.Size, partition.Version, partition.State)
}

func (c *OTACommand) upload(ctx context.Context, app *App, args []string) error {
	if len(args) > 1 {
		return &UsageError{Message: "ota upload accepts one optional local path"}
	}
	localPath := "-"
	if len(args) == 1 {
		localPath = args[0]
	}
	file, cleanup, err := openUploadSource(app.Stdin, localPath)
	if err != nil {
		return err
	}
	defer cleanup()
	metadata, err := file.Stat()
	if err != nil {
		return fmt.Errorf("inspect firmware: %w", err)
	}
	if metadata.Size() == 0 {
		return errors.New("firmware must not be empty")
	}
	hash := sha256.New()
	if _, err := io.Copy(hash, file); err != nil {
		return fmt.Errorf("hash firmware: %w", err)
	}
	checksum := fmt.Sprintf("%x", hash.Sum(nil))
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		return fmt.Errorf("rewind firmware: %w", err)
	}
	var token [16]byte
	if _, err := rand.Read(token[:]); err != nil {
		return fmt.Errorf("create upload ID: %w", err)
	}
	result, err := uploadWithDeviceProgress(ctx, app.API.OTA(), file,
		metadata.Size(), checksum, fmt.Sprintf("%x", token), app.Stderr, otaProgressInterval)
	if err != nil {
		return err
	}
	fmt.Fprintf(app.Stdout, "%s\t%s\t%d bytes\t%s\n", result.State, result.Firmware.Partition, metadata.Size(), result.SHA256)
	fmt.Fprintln(app.Stderr, "Run 'jsp32 reboot' to apply.")
	return nil
}

func (c *OTACommand) confirm(ctx context.Context, app *App, args []string) error {
	if len(args) != 0 {
		return &UsageError{Message: "ota confirm does not accept arguments"}
	}
	result, err := app.API.OTA().Confirm(ctx)
	if err != nil {
		return err
	}
	fmt.Fprint(app.Stdout, result.State)
	if result.Changed != nil {
		fmt.Fprintf(app.Stdout, "\tchanged=%t", *result.Changed)
	}
	fmt.Fprintln(app.Stdout)
	return nil
}

func (c *OTACommand) rollback(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("ota rollback", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	yes := flags.Bool("yes", false, "confirm rollback and reboot")
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			fmt.Fprintln(app.Stdout, "Usage: jsp32 ota rollback --yes")
			return nil
		}
		return &UsageError{Message: err.Error()}
	}
	if flags.NArg() != 0 {
		return &UsageError{Message: "ota rollback does not accept positional arguments"}
	}
	if !*yes {
		return &UsageError{Message: "ota rollback requires --yes"}
	}
	result, err := app.API.OTA().Rollback(ctx)
	if err != nil {
		return err
	}
	fmt.Fprintln(app.Stdout, result.State)
	return nil
}

func (c *OTACommand) printHelp(output io.Writer) {
	fmt.Fprintln(output, "Usage: jsp32 ota <command> [options]")
	fmt.Fprintln(output)
	fmt.Fprintln(output, "Commands:")
	fmt.Fprintln(output, "  status             show running, boot, and rollback state")
	fmt.Fprintln(output, "  upload [local]     stage firmware; local defaults to stdin (-)")
	fmt.Fprintln(output, "  confirm            mark the running firmware valid")
	fmt.Fprintln(output, "  rollback --yes     invalidate it and reboot to the previous firmware")
}
