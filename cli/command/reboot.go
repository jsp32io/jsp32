package command

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"io"
	"time"
)

type RebootCommand struct{}

func (c *RebootCommand) Name() string { return "reboot" }

func (c *RebootCommand) Summary() string {
	return "restart the device (boot staged firmware if present)"
}

func (c *RebootCommand) Run(ctx context.Context, app *App, args []string) error {
	flags := flag.NewFlagSet("reboot", flag.ContinueOnError)
	flags.SetOutput(io.Discard)
	wait := flags.Bool("wait", false, "wait for the device to finish restarting")
	if err := flags.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			fmt.Fprintln(app.Stdout, "Usage: jsp32 reboot [-wait]")
			return nil
		}
		return &UsageError{Message: err.Error()}
	}
	if flags.NArg() != 0 {
		return &UsageError{Message: "reboot does not accept positional arguments"}
	}
	var beforeUptime int64
	var beforeAt time.Time
	if *wait {
		probeCtx, cancel := context.WithTimeout(ctx, 500*time.Millisecond)
		info, err := app.API.System().Info(probeCtx)
		cancel()
		if err != nil {
			return fmt.Errorf("cannot check device uptime before reboot: %w", err)
		}
		beforeUptime = info.UptimeMs
		beforeAt = time.Now()
	}
	result, err := app.API.OTA().Reboot(ctx)
	if err != nil {
		return err
	}
	if *wait {
		if err := waitForReboot(ctx, app, beforeUptime, beforeAt); err != nil {
			return err
		}
		fmt.Fprintln(app.Stdout, "rebooted")
		return nil
	}
	fmt.Fprintln(app.Stdout, result.State)
	return nil
}

func waitForReboot(ctx context.Context, app *App, beforeUptime int64, beforeAt time.Time) error {
	waitCtx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()
	for {
		probeStarted := time.Now()
		probeCtx, stopProbe := context.WithTimeout(waitCtx, 500*time.Millisecond)
		info, err := app.API.System().Info(probeCtx)
		stopProbe()
		if err == nil {
			expectedUptime := beforeUptime + probeStarted.Sub(beforeAt).Milliseconds()
			if info.UptimeMs+100 < expectedUptime {
				return nil
			}
		}
		select {
		case <-waitCtx.Done():
			if ctx.Err() != nil {
				return ctx.Err()
			}
			return errors.New("timed out waiting for device to restart")
		case <-time.After(200 * time.Millisecond):
		}
	}
}
