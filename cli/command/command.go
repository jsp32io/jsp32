package command

import "context"

type Command interface {
	Name() string
	Summary() string
	Run(ctx context.Context, app *App, args []string) error
}

type UsageError struct {
	Message string
}

func (e *UsageError) Error() string {
	return e.Message
}
