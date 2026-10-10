package command

import (
	"context"
	"errors"
	"fmt"
	"io"
	"strconv"
)

type ConsoleCommand struct{}

func (c *ConsoleCommand) Name() string {
	return "console"
}

func (c *ConsoleCommand) Summary() string {
	return "stream JavaScript console output"
}

func (c *ConsoleCommand) Run(ctx context.Context, app *App, args []string) error {
	if len(args) != 0 {
		return &UsageError{Message: "console does not accept arguments"}
	}
	if app.API == nil {
		return errors.New("api client is nil")
	}

	stream, err := app.API.JS().Console(ctx)
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
		case "console", "message":
			fmt.Fprintln(app.Stdout, event.Data)
		case "dropped":
			count, parseErr := strconv.ParseUint(event.Data, 10, 64)
			if parseErr != nil {
				fmt.Fprintf(app.Stderr, "warning: console messages dropped: %s\n", event.Data)
			} else {
				fmt.Fprintf(app.Stderr, "warning: %d console messages dropped\n", count)
			}
		}
	}
}
