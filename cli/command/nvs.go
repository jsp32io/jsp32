package command

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"strconv"
	"strings"

	"github.com/jsp32io/jsp32/cli/api"
)

type NVSCommand struct{}

func (c *NVSCommand) Name() string { return "nvs" }

func (c *NVSCommand) Summary() string { return "manage typed device settings" }

func (c *NVSCommand) Run(ctx context.Context, app *App, args []string) error {
	if len(args) == 0 || args[0] == "help" || args[0] == "-h" || args[0] == "--help" {
		c.printHelp(app.Stdout)
		return nil
	}
	switch args[0] {
	case "info":
		return c.info(ctx, app, args[1:])
	case "namespaces":
		return c.namespaces(ctx, app, args[1:])
	case "ls":
		return c.list(ctx, app, args[1:])
	case "get":
		return c.get(ctx, app, args[1:])
	case "set":
		return c.set(ctx, app, args[1:])
	case "rm":
		return c.remove(ctx, app, args[1:])
	case "clear":
		return c.clear(ctx, app, args[1:])
	case "batch":
		return c.batch(ctx, app, args[1:])
	default:
		return &UsageError{Message: fmt.Sprintf("unknown nvs command %q", args[0])}
	}
}

func (c *NVSCommand) info(ctx context.Context, app *App, args []string) error {
	if len(args) != 0 {
		return &UsageError{Message: "nvs info does not accept arguments"}
	}
	info, err := app.API.NVS().Info(ctx)
	if err != nil {
		return err
	}
	fmt.Fprintf(app.Stdout, "partition\t%s\nsize\t%d\nencrypted\t%t\nused-entries\t%d\nfree-entries\t%d\ntotal-entries\t%d\nnamespaces\t%d\nmax-string-bytes\t%d\nmax-blob-bytes\t%d\nmax-batch-operations\t%d\n",
		info.Partition, info.Size, info.Encrypted, info.UsedEntries,
		info.FreeEntries, info.TotalEntries, info.NamespaceCount,
		info.Limits.MaxStringBytes, info.Limits.MaxBlobBytes,
		info.Limits.MaxBatchOperations)
	return nil
}

func (c *NVSCommand) namespaces(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("nvs namespaces", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	prefix := flags.String("prefix", "", "filter namespace names")
	help, err := parseNVSFlags(flags, args, app.Stdout, "Usage: jsp32 nvs namespaces [--prefix PREFIX]")
	if err != nil {
		return err
	}
	if help {
		return nil
	}
	if flags.NArg() != 0 {
		return &UsageError{Message: "nvs namespaces does not accept positional arguments"}
	}
	after := ""
	for {
		page, err := app.API.NVS().ListNamespaces(ctx, *prefix, 100, after)
		if err != nil {
			return err
		}
		for _, namespace := range page.Namespaces {
			fmt.Fprintf(app.Stdout, "%d\t%t\t%s\t%s\n", namespace.UsedEntries, namespace.Writable, namespace.Policy, namespace.Name)
		}
		if !page.HasMore || page.NextAfter == nil {
			return nil
		}
		if *page.NextAfter == "" || *page.NextAfter == after {
			return errors.New("NVS namespace list returned an invalid cursor")
		}
		after = *page.NextAfter
	}
}

func (c *NVSCommand) list(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("nvs ls", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	prefix := flags.String("prefix", "", "filter keys")
	help, err := parseNVSFlags(flags, args, app.Stdout, "Usage: jsp32 nvs ls [--prefix PREFIX] <namespace>")
	if err != nil {
		return err
	}
	if help {
		return nil
	}
	if flags.NArg() != 1 {
		return &UsageError{Message: "nvs ls requires exactly one namespace"}
	}
	after := ""
	for {
		page, err := app.API.NVS().ListEntries(ctx, flags.Arg(0), *prefix, 100, after)
		if err != nil {
			return err
		}
		for _, entry := range page.Entries {
			size := "-"
			if entry.Size != nil {
				size = strconv.FormatInt(*entry.Size, 10)
			}
			fmt.Fprintf(app.Stdout, "%s\t%s\t%t\t%t\t%s\n", entry.Type, size, entry.Readable, entry.Writable, entry.Key)
		}
		if !page.HasMore || page.NextAfter == nil {
			return nil
		}
		if *page.NextAfter == "" || *page.NextAfter == after {
			return errors.New("NVS entry list returned an invalid cursor")
		}
		after = *page.NextAfter
	}
}

func (c *NVSCommand) get(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("nvs get", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	raw := flags.Bool("raw", false, "write only the decoded value")
	help, err := parseNVSFlags(flags, args, app.Stdout, "Usage: jsp32 nvs get [--raw] <namespace> <key>")
	if err != nil {
		return err
	}
	if help {
		return nil
	}
	if flags.NArg() != 2 {
		return &UsageError{Message: "nvs get requires a namespace and key"}
	}
	value, err := app.API.NVS().Get(ctx, flags.Arg(0), flags.Arg(1))
	if err != nil {
		return err
	}
	if *raw {
		return writeRawNVSValue(app.Stdout, value)
	}
	return writeJSON(app.Stdout, value)
}

func (c *NVSCommand) set(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("nvs set", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	createOnly := flags.Bool("create", false, "fail if the key exists")
	match := flags.String("if-match", "", "update only if the current SHA-256 matches")
	usage := "Usage: jsp32 nvs set [--create | --if-match SHA256] <namespace> <key> <type> <value>"
	help, err := parseNVSFlags(flags, args, app.Stdout, usage)
	if err != nil {
		return err
	}
	if help {
		return nil
	}
	if flags.NArg() != 4 {
		return &UsageError{Message: "nvs set requires a namespace, key, type, and value"}
	}
	if *createOnly && *match != "" {
		return &UsageError{Message: "nvs set --create and --if-match are mutually exclusive"}
	}
	if *match != "" && !validSHA256(*match) {
		return &UsageError{Message: "nvs set --if-match must be 64 hexadecimal digits"}
	}
	payload, err := nvsValuePayload(flags.Arg(2), flags.Arg(3), app.Stdin)
	if err != nil {
		return &UsageError{Message: err.Error()}
	}
	value, err := app.API.NVS().Set(ctx, flags.Arg(0), flags.Arg(1), payload, api.NVSWriteOptions{
		CreateOnly: *createOnly, MatchSHA256: strings.ToLower(*match),
	})
	if err != nil {
		return err
	}
	return writeJSON(app.Stdout, value)
}

func (c *NVSCommand) remove(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("nvs rm", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	match := flags.String("if-match", "", "delete only if the current SHA-256 matches")
	help, err := parseNVSFlags(flags, args, app.Stdout, "Usage: jsp32 nvs rm [--if-match SHA256] <namespace> <key>")
	if err != nil {
		return err
	}
	if help {
		return nil
	}
	if flags.NArg() != 2 {
		return &UsageError{Message: "nvs rm requires a namespace and key"}
	}
	if *match != "" && !validSHA256(*match) {
		return &UsageError{Message: "nvs rm --if-match must be 64 hexadecimal digits"}
	}
	if err := app.API.NVS().Delete(ctx, flags.Arg(0), flags.Arg(1), strings.ToLower(*match)); err != nil {
		return err
	}
	fmt.Fprintf(app.Stdout, "%s/%s\n", flags.Arg(0), flags.Arg(1))
	return nil
}

func (c *NVSCommand) clear(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("nvs clear", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	yes := flags.Bool("yes", false, "confirm deletion of the entire namespace")
	help, err := parseNVSFlags(flags, args, app.Stdout, "Usage: jsp32 nvs clear --yes <namespace>")
	if err != nil {
		return err
	}
	if help {
		return nil
	}
	if flags.NArg() != 1 {
		return &UsageError{Message: "nvs clear requires exactly one namespace"}
	}
	if !*yes {
		return &UsageError{Message: "nvs clear requires --yes"}
	}
	if err := app.API.NVS().Clear(ctx, flags.Arg(0)); err != nil {
		return err
	}
	fmt.Fprintln(app.Stdout, flags.Arg(0))
	return nil
}

func (c *NVSCommand) batch(ctx context.Context, app *App, args []string) error {
	if len(args) < 1 || len(args) > 2 {
		return &UsageError{Message: "nvs batch requires a namespace and optional local path"}
	}
	reader := app.Stdin
	var file *os.File
	if len(args) == 2 && args[1] != "-" {
		var err error
		file, err = os.Open(args[1])
		if err != nil {
			return fmt.Errorf("open batch file: %w", err)
		}
		defer file.Close()
		reader = file
	}
	body, err := io.ReadAll(io.LimitReader(reader, 1024*1024+1))
	if err != nil {
		return fmt.Errorf("read batch JSON: %w", err)
	}
	if len(body) > 1024*1024 {
		return errors.New("batch JSON exceeds 1 MiB")
	}
	result, err := app.API.NVS().Batch(ctx, args[0], body)
	if err != nil {
		return err
	}
	return writeJSON(app.Stdout, result)
}

func nvsValuePayload(valueType, text string, stdin io.Reader) (map[string]any, error) {
	payload := map[string]any{"type": valueType}
	switch valueType {
	case "i8":
		value, err := strconv.ParseInt(text, 10, 8)
		if err != nil {
			return nil, fmt.Errorf("invalid i8 value %q", text)
		}
		payload["value"] = value
	case "u8":
		value, err := strconv.ParseUint(text, 10, 8)
		if err != nil {
			return nil, fmt.Errorf("invalid u8 value %q", text)
		}
		payload["value"] = value
	case "i16":
		value, err := strconv.ParseInt(text, 10, 16)
		if err != nil {
			return nil, fmt.Errorf("invalid i16 value %q", text)
		}
		payload["value"] = value
	case "u16":
		value, err := strconv.ParseUint(text, 10, 16)
		if err != nil {
			return nil, fmt.Errorf("invalid u16 value %q", text)
		}
		payload["value"] = value
	case "i32":
		value, err := strconv.ParseInt(text, 10, 32)
		if err != nil {
			return nil, fmt.Errorf("invalid i32 value %q", text)
		}
		payload["value"] = value
	case "u32":
		value, err := strconv.ParseUint(text, 10, 32)
		if err != nil {
			return nil, fmt.Errorf("invalid u32 value %q", text)
		}
		payload["value"] = value
	case "i64":
		if _, err := strconv.ParseInt(text, 10, 64); err != nil {
			return nil, fmt.Errorf("invalid i64 value %q", text)
		}
		payload["value"] = text
	case "u64":
		if _, err := strconv.ParseUint(text, 10, 64); err != nil {
			return nil, fmt.Errorf("invalid u64 value %q", text)
		}
		payload["value"] = text
	case "float":
		value, err := strconv.ParseFloat(text, 32)
		if err != nil || math.IsInf(value, 0) || math.IsNaN(value) {
			return nil, fmt.Errorf("invalid float value %q", text)
		}
		payload["value"] = value
	case "double":
		value, err := strconv.ParseFloat(text, 64)
		if err != nil || math.IsInf(value, 0) || math.IsNaN(value) {
			return nil, fmt.Errorf("invalid double value %q", text)
		}
		payload["value"] = value
	case "string":
		payload["value"] = text
	case "blob":
		data, err := readBlobValue(text, stdin)
		if err != nil {
			return nil, err
		}
		payload["encoding"] = "base64"
		payload["value"] = base64.StdEncoding.EncodeToString(data)
	default:
		return nil, fmt.Errorf("unsupported NVS type %q", valueType)
	}
	return payload, nil
}

func readBlobValue(path string, stdin io.Reader) ([]byte, error) {
	if path == "-" {
		data, err := io.ReadAll(io.LimitReader(stdin, 1024*1024+1))
		if err != nil {
			return nil, fmt.Errorf("read blob from standard input: %w", err)
		}
		if len(data) > 1024*1024 {
			return nil, errors.New("blob exceeds 1 MiB")
		}
		return data, nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("read blob file: %w", err)
	}
	return data, nil
}

func writeRawNVSValue(output io.Writer, value *api.NVSValue) error {
	if value.Type == "blob" {
		var encoded string
		if err := json.Unmarshal(value.Value, &encoded); err != nil {
			return fmt.Errorf("decode blob value: %w", err)
		}
		decoded, err := base64.StdEncoding.DecodeString(encoded)
		if err != nil {
			return fmt.Errorf("decode blob value: %w", err)
		}
		_, err = output.Write(decoded)
		return err
	}
	if value.Type == "string" || value.Type == "i64" || value.Type == "u64" {
		var text string
		if err := json.Unmarshal(value.Value, &text); err != nil {
			return fmt.Errorf("decode NVS value: %w", err)
		}
		if value.Type == "string" {
			_, err := io.WriteString(output, text)
			return err
		}
		_, err := fmt.Fprintln(output, text)
		return err
	}
	_, err := fmt.Fprintln(output, string(value.Value))
	return err
}

func writeJSON(output io.Writer, value any) error {
	encoder := json.NewEncoder(output)
	encoder.SetEscapeHTML(false)
	return encoder.Encode(value)
}

func parseNVSFlags(flags *flag.FlagSet, args []string, output io.Writer, usage string) (bool, error) {
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			fmt.Fprintln(output, usage)
			return true, nil
		}
		return false, &UsageError{Message: err.Error()}
	}
	return false, nil
}

func (c *NVSCommand) printHelp(output io.Writer) {
	fmt.Fprintln(output, "Usage: jsp32 nvs <command> [options]")
	fmt.Fprintln(output)
	fmt.Fprintln(output, "Commands:")
	fmt.Fprintln(output, "  info                                      show NVS capacity and limits")
	fmt.Fprintln(output, "  namespaces [--prefix PREFIX]              list visible namespaces")
	fmt.Fprintln(output, "  ls [--prefix PREFIX] <namespace>           list entries")
	fmt.Fprintln(output, "  get [--raw] <namespace> <key>              read a typed value")
	fmt.Fprintln(output, "  set [options] <namespace> <key> <type> <v> set a typed value")
	fmt.Fprintln(output, "  rm [options] <namespace> <key>             delete an entry")
	fmt.Fprintln(output, "  clear --yes <namespace>                    delete a namespace")
	fmt.Fprintln(output, "  batch <namespace> [local]                  apply atomic JSON operations")
}
