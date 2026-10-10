package command

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"time"

	"github.com/jsp32io/jsp32/cli/api"
)

type EvalCommand struct{}

func (c *EvalCommand) Name() string {
	return "eval"
}

func (c *EvalCommand) Summary() string {
	return "evaluate JavaScript on the device"
}

func (c *EvalCommand) Run(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet(c.Name(), flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	mode := flags.String("mode", string(api.EvalModeExpr), "evaluation mode: expr or script")
	awaitResult := flags.Bool("await", true, "await a returned Promise")
	timeout := flags.Duration("timeout", 5*time.Second, "evaluation timeout")
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			c.printHelp(app.Stdout)
			return nil
		}
		return &UsageError{Message: err.Error()}
	}
	if flags.NArg() != 1 {
		return &UsageError{Message: "eval requires exactly one JavaScript source argument"}
	}
	if *mode != string(api.EvalModeExpr) && *mode != string(api.EvalModeScript) {
		return &UsageError{Message: "eval mode must be expr or script"}
	}
	if *timeout < time.Millisecond {
		return &UsageError{Message: "eval timeout must be at least 1ms"}
	}
	if app.API == nil {
		return errors.New("api client is nil")
	}

	request := &api.EvalRequest{
		Source:    flags.Arg(0),
		Mode:      api.EvalMode(*mode),
		Await:     *awaitResult,
		TimeoutMS: timeout.Milliseconds(),
	}
	requestContext, cancel := context.WithTimeout(ctx, *timeout+2*time.Second)
	defer cancel()
	result, err := app.API.JS().Eval(requestContext, request)
	if err != nil {
		return err
	}
	if !result.OK {
		if result.Error != nil {
			if result.Error.Stack != "" {
				return fmt.Errorf("%w\n%s", result.Error, result.Error.Stack)
			}
			return result.Error
		}
		return errors.New("JavaScript evaluation failed")
	}

	display := result.Display
	if display == "" {
		if result.Type == "string" {
			var value string
			if json.Unmarshal(result.Value, &value) == nil {
				display = value
			}
		} else if len(result.Value) > 0 && string(result.Value) != "null" {
			display = string(result.Value)
		} else if result.Type != "" {
			display = result.Type
		} else {
			display = "undefined"
		}
	}
	fmt.Fprintln(app.Stdout, display)
	return nil
}

func (c *EvalCommand) printHelp(output io.Writer) {
	fmt.Fprintln(output, "Usage: jsp32 eval [--mode expr|script] [--await=false] [--timeout 5s] <source>")
}
