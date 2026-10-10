package main

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"github.com/jsp32io/jsp32/cli/api"
	"github.com/jsp32io/jsp32/cli/command"
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	app := &command.App{
		API: &api.Client{
			BaseURL: os.Getenv("JSP32_BASE_URL"),
		},
		Stdout: os.Stdout,
		Stderr: os.Stderr,
	}

	if err := app.Run(ctx, os.Args[1:]); err != nil {
		fmt.Fprintf(os.Stderr, "jsp32: %v\n", err)
		var usageError *command.UsageError
		if errors.As(err, &usageError) {
			os.Exit(2)
		}
		os.Exit(1)
	}
}
