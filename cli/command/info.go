package command

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
)

type InfoCommand struct{}

func (*InfoCommand) Name() string    { return "info" }
func (*InfoCommand) Summary() string { return "show device system information as JSON" }

func (*InfoCommand) Run(ctx context.Context, app *App, args []string) error {
	if len(args) == 1 && (args[0] == "help" || args[0] == "-h" || args[0] == "--help") {
		_, err := io.WriteString(app.Stdout, "Usage: jsp32 info\n\nShow hardware, firmware, uptime, network, memory and partition information as JSON.\n")
		return err
	}
	if len(args) != 0 {
		return &UsageError{Message: "info does not accept arguments"}
	}
	info, err := app.API.System().Info(ctx)
	if err != nil {
		return err
	}
	var output bytes.Buffer
	if err := json.Indent(&output, info.Raw, "", "  "); err != nil {
		return err
	}
	output.WriteByte('\n')
	_, err = output.WriteTo(app.Stdout)
	return err
}
