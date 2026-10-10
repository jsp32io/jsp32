package command

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"

	"github.com/jsp32io/jsp32/cli/api"
)

type RegCommand struct{}

func (*RegCommand) Name() string    { return "reg" }
func (*RegCommand) Summary() string { return "read, write and watch X/Y/M/D registers" }

func (*RegCommand) Run(ctx context.Context, app *App, args []string) error {
	if len(args) == 0 || args[0] == "help" || args[0] == "-h" || args[0] == "--help" {
		fmt.Fprintln(app.Stdout, "Usage: jsp32 reg info | get ADDRESS... [--json] | read START COUNT [--json] | set ADDRESS=VALUE... | write START VALUE... | watch [ADDRESS...] [--json]")
		return nil
	}
	if app.API == nil {
		return errors.New("api client is nil")
	}
	command := args[0]
	args = args[1:]
	jsonOutput := false
	filtered := args[:0]
	for _, arg := range args {
		if arg == "--json" {
			jsonOutput = true
		} else {
			filtered = append(filtered, arg)
		}
	}
	args = filtered
	if jsonOutput && command != "get" && command != "read" && command != "watch" {
		return &UsageError{Message: "--json is supported by reg get, read and watch"}
	}
	r := app.API.Reg()
	switch command {
	case "info":
		if len(args) != 0 {
			return &UsageError{Message: "reg info accepts no arguments"}
		}
		info, err := r.Info(ctx)
		if err != nil {
			return err
		}
		fmt.Fprintf(app.Stdout, "revision\t%s\nX\t%d\nY\t%d\nM\t%d\nD\t%d\nmax-transaction\t%d\nvolatile\t%t\n", info.Revision, info.Areas["X"], info.Areas["Y"], info.Areas["M"], info.Areas["D"], info.MaxTransaction, info.Volatile)
		return nil
	case "get":
		if len(args) == 0 {
			return &UsageError{Message: "reg get requires one or more addresses"}
		}
		for _, address := range args {
			if err := validRegAddress(address, false); err != nil {
				return err
			}
		}
		result, err := r.Read(ctx, args)
		if err != nil {
			return err
		}
		return printRegSnapshot(app.Stdout, result, jsonOutput)
	case "read":
		if len(args) != 2 {
			return &UsageError{Message: "reg read requires START COUNT"}
		}
		if err := validRegAddress(args[0], false); err != nil {
			return err
		}
		count, err := strconv.Atoi(args[1])
		if err != nil || count < 1 {
			return &UsageError{Message: "COUNT must be a positive integer"}
		}
		result, err := r.ReadRange(ctx, args[0], count)
		if err != nil {
			return err
		}
		return printRegSnapshot(app.Stdout, result, jsonOutput)
	case "set":
		if len(args) == 0 {
			return &UsageError{Message: "reg set requires ADDRESS=VALUE pairs"}
		}
		writes := make([]api.RegWrite, 0, len(args))
		for _, arg := range args {
			address, raw, ok := strings.Cut(arg, "=")
			if !ok {
				return &UsageError{Message: "expected ADDRESS=VALUE"}
			}
			if err := validRegAddress(address, true); err != nil {
				return err
			}
			value, err := parseRegValue(address, raw)
			if err != nil {
				return err
			}
			writes = append(writes, api.RegWrite{Address: address, Value: value})
		}
		result, err := r.Write(ctx, writes)
		if err != nil {
			return err
		}
		fmt.Fprintf(app.Stdout, "revision\t%s\n", result.Revision)
		return nil
	case "write":
		if len(args) < 2 {
			return &UsageError{Message: "reg write requires START and at least one VALUE"}
		}
		if err := validRegAddress(args[0], true); err != nil {
			return err
		}
		values := make([]any, 0, len(args)-1)
		for _, raw := range args[1:] {
			value, err := parseRegValue(args[0], raw)
			if err != nil {
				return err
			}
			values = append(values, value)
		}
		result, err := r.WriteRange(ctx, args[0], values)
		if err != nil {
			return err
		}
		fmt.Fprintf(app.Stdout, "revision\t%s\n", result.Revision)
		return nil
	case "watch":
		for _, address := range args {
			if err := validRegAddress(address, false); err != nil {
				return err
			}
		}
		stream, err := r.Watch(ctx, args...)
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			return err
		}
		defer stream.Close()
		for {
			event, err := stream.Next()
			if err != nil {
				if errors.Is(err, io.EOF) || ctx.Err() != nil {
					return nil
				}
				return err
			}
			switch event.Type {
			case "dropped":
				fmt.Fprintf(app.Stderr, "warning: %s register changes dropped\n", event.Data)
			case "change":
				var change api.RegChange
				if err := json.Unmarshal([]byte(event.Data), &change); err != nil {
					return fmt.Errorf("decode register change: %w", err)
				}
				if change.Revision == "" || validRegAddress(change.Address, false) != nil || len(change.Value) == 0 {
					return errors.New("invalid register change event")
				}
				if jsonOutput {
					fmt.Fprintln(app.Stdout, event.Data)
				} else {
					fmt.Fprintf(app.Stdout, "%s\t%s\t%s\n", change.Revision, change.Address, formatRegJSON(change.Value))
				}
			}
		}
	default:
		return &UsageError{Message: fmt.Sprintf("unknown reg command %q", command)}
	}
}

func validRegAddress(address string, writing bool) error {
	if len(address) < 2 || !strings.ContainsRune("XYMD", rune(address[0])) {
		return &UsageError{Message: fmt.Sprintf("invalid register address %q", address)}
	}
	index := address[1:]
	if len(index) > 1 && index[0] == '0' {
		return &UsageError{Message: fmt.Sprintf("invalid register address %q", address)}
	}
	for _, char := range index {
		if char < '0' || char > '9' {
			return &UsageError{Message: fmt.Sprintf("invalid register address %q", address)}
		}
	}
	if _, err := strconv.ParseUint(index, 10, 16); err != nil {
		return &UsageError{Message: fmt.Sprintf("invalid register address %q", address)}
	}
	if writing && address[0] == 'X' {
		return &UsageError{Message: "X registers are read-only"}
	}
	return nil
}

func parseRegValue(address, raw string) (any, error) {
	if address[0] != 'D' {
		switch strings.ToLower(raw) {
		case "1", "true":
			return true, nil
		case "0", "false":
			return false, nil
		default:
			return nil, &UsageError{Message: fmt.Sprintf("%s requires 0, 1, true or false", address)}
		}
	}
	base := 10
	if strings.HasPrefix(raw, "0x") || strings.HasPrefix(raw, "0X") {
		base = 0
	}
	value, err := strconv.ParseUint(raw, base, 16)
	if err != nil {
		return nil, &UsageError{Message: fmt.Sprintf("%s requires a number from 0 to 65535", address)}
	}
	return value, nil
}

func printRegSnapshot(out io.Writer, snapshot *api.RegSnapshot, jsonOutput bool) error {
	if jsonOutput {
		return json.NewEncoder(out).Encode(snapshot)
	}
	for _, value := range snapshot.Values {
		fmt.Fprintf(out, "%s\t%s\n", value.Address, formatRegJSON(value.Value))
	}
	return nil
}

func formatRegJSON(value json.RawMessage) string {
	switch string(value) {
	case "true":
		return "1"
	case "false":
		return "0"
	}
	return string(value)
}
