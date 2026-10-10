package command

import (
	"context"
	"crypto/sha256"
	"errors"
	"flag"
	"fmt"
	"io"
	"mime"
	"os"
	"path/filepath"
	"strings"

	"github.com/jsp32io/jsp32/cli/api"
)

type FSCommand struct{}

func (c *FSCommand) Name() string { return "fs" }

func (c *FSCommand) Summary() string { return "manage files on the device" }

func (c *FSCommand) Run(ctx context.Context, app *App, args []string) error {
	if len(args) == 0 || args[0] == "help" || args[0] == "-h" || args[0] == "--help" {
		c.printHelp(app.Stdout)
		return nil
	}
	switch args[0] {
	case "info":
		return c.info(ctx, app, args[1:])
	case "ls":
		return c.list(ctx, app, args[1:])
	case "stat":
		return c.stat(ctx, app, args[1:])
	case "get":
		return c.get(ctx, app, args[1:])
	case "put":
		return c.put(ctx, app, args[1:])
	case "mkdir":
		return c.mkdir(ctx, app, args[1:])
	case "rm":
		return c.remove(ctx, app, args[1:])
	default:
		return &UsageError{Message: fmt.Sprintf("unknown fs command %q", args[0])}
	}
}

func (c *FSCommand) info(ctx context.Context, app *App, args []string) error {
	if len(args) != 0 {
		return &UsageError{Message: "fs info does not accept arguments"}
	}
	info, err := app.API.FS().Info(ctx)
	if err != nil {
		return err
	}
	fmt.Fprintf(app.Stdout, "type\t%s\ntotal\t%d\nused\t%d\nfree\t%d\nmax-name-bytes\t%d\ndirectories\t%t\natomic-replace\t%t\n",
		info.Type, info.TotalBytes, info.UsedBytes, info.FreeBytes, info.MaxNameBytes,
		info.Directories, info.AtomicReplace)
	return nil
}

func (c *FSCommand) list(ctx context.Context, app *App, args []string) error {
	if len(args) > 1 {
		return &UsageError{Message: "fs ls accepts one optional directory path"}
	}
	directory := "/"
	if len(args) == 1 {
		directory = args[0]
	}
	after := ""
	for {
		page, err := app.API.FS().List(ctx, directory, 100, after)
		if err != nil {
			return err
		}
		for _, file := range page.Files {
			if file.Directory {
				fmt.Fprintf(app.Stdout, "dir\t%s\n", file.Path)
			} else {
				fmt.Fprintf(app.Stdout, "%d\t%s\n", file.Size, file.Path)
			}
		}
		if page.NextAfter == nil {
			return nil
		}
		if *page.NextAfter == "" || *page.NextAfter == after {
			return errors.New("filesystem list returned an invalid cursor")
		}
		after = *page.NextAfter
	}
}

func (c *FSCommand) mkdir(ctx context.Context, app *App, args []string) error {
	if len(args) != 1 {
		return &UsageError{Message: "fs mkdir requires exactly one device directory path"}
	}
	if err := app.API.FS().Mkdir(ctx, args[0]); err != nil {
		return err
	}
	fmt.Fprintln(app.Stdout, args[0])
	return nil
}

func (c *FSCommand) stat(ctx context.Context, app *App, args []string) error {
	if len(args) != 1 {
		return &UsageError{Message: "fs stat requires exactly one device path"}
	}
	info, err := app.API.FS().Stat(ctx, args[0])
	if err != nil {
		return err
	}
	fmt.Fprintf(app.Stdout, "path\t%s\nsize\t%d\nsha256\t%s\n", info.Path, info.Size, info.SHA256)
	return nil
}

func (c *FSCommand) get(ctx context.Context, app *App, args []string) error {
	if len(args) < 1 || len(args) > 2 {
		return &UsageError{Message: "fs get requires a device path and optional local path"}
	}
	download, err := app.API.FS().Read(ctx, args[0])
	if err != nil {
		return err
	}
	defer download.Close()
	localPath := "-"
	if len(args) == 2 {
		localPath = args[1]
	}
	if localPath == "-" {
		hash := sha256.New()
		written, err := io.Copy(io.MultiWriter(app.Stdout, hash), download.Body)
		if err != nil {
			return fmt.Errorf("download file: %w", err)
		}
		return verifyDownload(written, fmt.Sprintf("%x", hash.Sum(nil)), download)
	}

	directory := filepath.Dir(localPath)
	temporary, err := os.CreateTemp(directory, ".jsp32-download-*")
	if err != nil {
		return fmt.Errorf("create temporary download: %w", err)
	}
	temporaryPath := temporary.Name()
	committed := false
	defer func() {
		temporary.Close()
		if !committed {
			_ = os.Remove(temporaryPath)
		}
	}()
	hash := sha256.New()
	written, err := io.Copy(io.MultiWriter(temporary, hash), download.Body)
	if err != nil {
		return fmt.Errorf("download file: %w", err)
	}
	checksum := fmt.Sprintf("%x", hash.Sum(nil))
	if err := verifyDownload(written, checksum, download); err != nil {
		return err
	}
	if err := temporary.Close(); err != nil {
		return fmt.Errorf("close downloaded file: %w", err)
	}
	if err := os.Rename(temporaryPath, localPath); err != nil {
		return fmt.Errorf("replace local file: %w", err)
	}
	committed = true
	fmt.Fprintf(app.Stdout, "%s\t%d bytes\t%s\n", localPath, download.Size, download.SHA256)
	return nil
}

func verifyDownload(written int64, checksum string, download *api.FileDownload) error {
	if written != download.Size {
		return fmt.Errorf("download file: received %d bytes, expected %d", written, download.Size)
	}
	if checksum != download.SHA256 {
		return fmt.Errorf("download file: SHA-256 mismatch (received %s, expected %s)", checksum, download.SHA256)
	}
	return nil
}

func (c *FSCommand) put(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("fs put", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	createOnly := flags.Bool("create", false, "fail if the device file exists")
	match := flags.String("if-match", "", "replace only if the current SHA-256 matches")
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			fmt.Fprintln(app.Stdout, "Usage: jsp32 fs put [--create | --if-match SHA256] <device-path> [local-path]")
			return nil
		}
		return &UsageError{Message: err.Error()}
	}
	if flags.NArg() < 1 || flags.NArg() > 2 {
		return &UsageError{Message: "fs put requires a device path and optional local path"}
	}
	if *createOnly && *match != "" {
		return &UsageError{Message: "fs put --create and --if-match are mutually exclusive"}
	}
	if *match != "" && !validSHA256(*match) {
		return &UsageError{Message: "fs put --if-match must be 64 hexadecimal digits"}
	}

	devicePath := flags.Arg(0)
	localPath := "-"
	if flags.NArg() == 2 {
		localPath = flags.Arg(1)
	}
	file, cleanup, err := openUploadSource(app.Stdin, localPath)
	if err != nil {
		return err
	}
	defer cleanup()
	metadata, err := file.Stat()
	if err != nil {
		return fmt.Errorf("inspect upload source: %w", err)
	}
	hash := sha256.New()
	if _, err := io.Copy(hash, file); err != nil {
		return fmt.Errorf("hash local file: %w", err)
	}
	checksum := fmt.Sprintf("%x", hash.Sum(nil))
	if _, err := file.Seek(0, io.SeekStart); err != nil {
		return fmt.Errorf("rewind local file: %w", err)
	}
	contentType := mime.TypeByExtension(strings.ToLower(filepath.Ext(devicePath)))
	result, err := app.API.FS().Put(ctx, devicePath, file, metadata.Size(), api.PutOptions{
		CreateOnly:    *createOnly,
		MatchSHA256:   strings.ToLower(*match),
		ContentSHA256: checksum,
		ContentType:   contentType,
	})
	if err != nil {
		return err
	}
	action := "updated"
	if result.Created {
		action = "created"
	}
	fmt.Fprintf(app.Stdout, "%s\t%s\t%d bytes\t%s\n", action, result.Path, result.Size, result.SHA256)
	if result.RestartRequired {
		fmt.Fprintln(app.Stderr, "warning: restart the device to activate /main.js")
	}
	return nil
}

func openUploadSource(stdin io.Reader, localPath string) (*os.File, func(), error) {
	if localPath != "-" {
		file, err := os.Open(localPath)
		if err != nil {
			return nil, func() {}, fmt.Errorf("open local file: %w", err)
		}
		metadata, err := file.Stat()
		if err != nil {
			file.Close()
			return nil, func() {}, fmt.Errorf("inspect local file: %w", err)
		}
		if !metadata.Mode().IsRegular() {
			file.Close()
			return nil, func() {}, errors.New("local upload source must be a regular file")
		}
		return file, func() { _ = file.Close() }, nil
	}

	temporary, err := os.CreateTemp("", "jsp32-upload-*")
	if err != nil {
		return nil, func() {}, fmt.Errorf("create temporary upload: %w", err)
	}
	cleanup := func() {
		_ = temporary.Close()
		_ = os.Remove(temporary.Name())
	}
	if _, err := io.Copy(temporary, stdin); err != nil {
		cleanup()
		return nil, func() {}, fmt.Errorf("read upload from standard input: %w", err)
	}
	if _, err := temporary.Seek(0, io.SeekStart); err != nil {
		cleanup()
		return nil, func() {}, fmt.Errorf("rewind temporary upload: %w", err)
	}
	return temporary, cleanup, nil
}

func (c *FSCommand) remove(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("fs rm", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	match := flags.String("if-match", "", "delete only if the current SHA-256 matches")
	recursive := flags.Bool("r", false, "remove a non-empty directory and its contents")
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			fmt.Fprintln(app.Stdout, "Usage: jsp32 fs rm [-r | --if-match SHA256] <device-path>")
			return nil
		}
		return &UsageError{Message: err.Error()}
	}
	if flags.NArg() != 1 {
		return &UsageError{Message: "fs rm requires exactly one device path"}
	}
	if *match != "" && !validSHA256(*match) {
		return &UsageError{Message: "fs rm --if-match must be 64 hexadecimal digits"}
	}
	if *recursive && *match != "" {
		return &UsageError{Message: "fs rm -r and --if-match are mutually exclusive"}
	}
	path := flags.Arg(0)
	var err error
	if *recursive {
		err = removeRecursive(ctx, app.API.FS(), path)
	} else {
		err = app.API.FS().Delete(ctx, path, strings.ToLower(*match))
	}
	if err != nil {
		return err
	}
	fmt.Fprintln(app.Stdout, path)
	return nil
}

func removeRecursive(ctx context.Context, filesystem *api.FS, path string) error {
	if path == "/" {
		return &UsageError{Message: "fs rm -r cannot remove the filesystem root"}
	}
	err := filesystem.Delete(ctx, path, "")
	if err == nil {
		return nil
	}
	var httpError *api.HTTPError
	if !errors.As(err, &httpError) || httpError.Detail == nil ||
		httpError.Detail.Code != "FS_DIRECTORY_NOT_EMPTY" {
		return err
	}
	for {
		page, err := filesystem.List(ctx, path, 100, "")
		if err != nil {
			return err
		}
		if len(page.Files) == 0 {
			break
		}
		for _, entry := range page.Files {
			if err := removeRecursive(ctx, filesystem, entry.Path); err != nil {
				return err
			}
		}
	}
	return filesystem.Delete(ctx, path, "")
}

func validSHA256(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, character := range value {
		if !strings.ContainsRune("0123456789abcdefABCDEF", character) {
			return false
		}
	}
	return true
}

func (c *FSCommand) printHelp(output io.Writer) {
	fmt.Fprintln(output, "Usage: jsp32 fs <command> [options]")
	fmt.Fprintln(output)
	fmt.Fprintln(output, "Commands:")
	fmt.Fprintln(output, "  info                         show filesystem capacity and capabilities")
	fmt.Fprintln(output, "  ls [directory]               list files and directories")
	fmt.Fprintln(output, "  stat <device-path>           show file size and SHA-256")
	fmt.Fprintln(output, "  get <path> [local]           download; local defaults to stdout (-)")
	fmt.Fprintln(output, "  put [options] <path> [local] upload; local defaults to stdin (-)")
	fmt.Fprintln(output, "  mkdir <directory>            create a directory")
	fmt.Fprintln(output, "  rm [-r] <device-path>        delete a file or directory")
}
