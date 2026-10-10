package command

import (
	"context"
	"io"

	"github.com/jsp32io/jsp32/cli/api"
)

type ManCommand struct{}

func (c *ManCommand) Name() string    { return "man" }
func (c *ManCommand) Summary() string { return "read device manuals as Markdown" }

func (c *ManCommand) Run(ctx context.Context, app *App, args []string) error {
	if len(args) == 1 && (args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
		_, err := io.WriteString(app.Stdout, "Usage: jsp32 man [topic]\n\nDefault: index\nExample: jsp32 man js.gpio\n")
		return err
	}
	if len(args) > 1 {
		return &UsageError{Message: "usage: jsp32 man [topic]"}
	}
	topic := "index"
	if len(args) == 1 {
		topic = args[0]
	}
	if !api.ValidManualTopic(topic) {
		return &UsageError{Message: "invalid manual topic; use a dotted name such as js.gpio"}
	}
	markdown, err := app.API.Manual(ctx, topic)
	if err != nil {
		return err
	}
	_, err = io.WriteString(app.Stdout, markdown)
	return err
}
