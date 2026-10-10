package command

import (
	"context"
	"fmt"
	"io"
	"os"

	"github.com/jsp32io/jsp32/cli/api"
)

type App struct {
	API      *api.Client
	Stdin    io.Reader
	Stdout   io.Writer
	Stderr   io.Writer
	Commands []Command
}

func (a *App) Run(ctx context.Context, args []string) error {
	if a.Stdin == nil {
		a.Stdin = os.Stdin
	}
	if a.Stdout == nil {
		a.Stdout = os.Stdout
	}
	if a.Stderr == nil {
		a.Stderr = os.Stderr
	}
	if len(a.Commands) == 0 {
		a.Commands = []Command{&InfoCommand{}, &ManCommand{}, &EvalCommand{}, &ConsoleCommand{}, &FSCommand{}, &RegCommand{}, &NVSCommand{}, &OTACommand{}, &RebootCommand{}}
	}

	if len(args) == 0 || args[0] == "help" || args[0] == "-h" || args[0] == "--help" {
		a.printHelp()
		return nil
	}
	for _, candidate := range a.Commands {
		if candidate.Name() == args[0] {
			return candidate.Run(ctx, a, args[1:])
		}
	}
	return &UsageError{Message: fmt.Sprintf("unknown command %q", args[0])}
}

func (a *App) printHelp() {
	fmt.Fprintln(a.Stdout, "Usage: jsp32 <command> [options]")
	fmt.Fprintln(a.Stdout)
	fmt.Fprintln(a.Stdout, "Commands:")
	for _, command := range a.Commands {
		fmt.Fprintf(a.Stdout, "  %-10s %s\n", command.Name(), command.Summary())
	}
	fmt.Fprintln(a.Stdout)
	fmt.Fprintln(a.Stdout, "Environment:")
	fmt.Fprintln(a.Stdout, "  JSP32_BASE_URL  Device HTTP(S) URL; required for device commands.")
	fmt.Fprintln(a.Stdout, "                  Example: http://192.168.1.20")
	fmt.Fprintln(a.Stdout, "                  /api/v1 is appended unless the URL ends in /api/vN.")
	fmt.Fprintln(a.Stdout)
	fmt.Fprintln(a.Stdout, "Examples:")
	fmt.Fprintln(a.Stdout, "  export JSP32_BASE_URL=http://192.168.1.20")
	fmt.Fprintln(a.Stdout, "  jsp32 info")
	fmt.Fprintln(a.Stdout, "  JSP32_BASE_URL=http://192.168.1.20/api/v1 jsp32 man js")
}
