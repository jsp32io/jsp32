# Register HTTP API

The `/api/v1/registers` endpoints operate on the native RAM register table.
The API does not evaluate JavaScript. Register addresses use an uppercase area
letter (`X`, `Y`, `M`, or `D`) and a zero-based decimal index, without leading
zeroes. The table is volatile; all values reset at boot.

| Method | Path | Purpose |
| --- | --- | --- |
| GET | `/api/v1/registers` | Capacities, revision, maximum transaction size |
| POST | `/api/v1/registers/read` | Atomic snapshot of selected or contiguous registers |
| POST | `/api/v1/registers/write` | Atomic write of selected or contiguous registers |
| GET | `/api/v1/registers/events` | Server-sent change events, optionally filtered by address or range |

Read bodies are either `{"addresses":["X0","D100"]}` or
`{"start":"D100","count":8}`. Responses contain a decimal string `revision`
and ordered `values` entries. Write bodies are either
`{"writes":[{"address":"M10","value":true},{"address":"D100","value":42}]}`
or `{"start":"D100","values":[10,20,30]}`. A successful write returns its
decimal string `revision`. Each request permits 1 through `maxTransaction`
registers. `X` cannot be written through HTTP. Bit values are JSON booleans or
numbers 0/1; `D` values are integer numbers 0 through 65535. Invalid requests
leave the table unchanged.

The event stream sends compact `change` records with `r` (revision), `a`
(address), and `v` (new value), plus `dropped` records when a slow reader misses
changes. There is a bounded event ring and a configured limit on concurrent
watchers. A no-op write does not increment the revision or emit a change.

The event stream accepts these optional query parameters:

| Query | Subscription |
| --- | --- |
| No parameters | All register changes (existing behavior) |
| `addresses=D100,D101` | The listed addresses |
| `start=D100&count=32` | D100 through D131, inclusive |
| `addresses=M10&start=D100&count=32` | Union of the list and range |

For example, `/api/v1/registers/events?start=D100&count=32`
sends changes in this form:

```text
event: change
data: {"r":"123","a":"D100","v":42}
```

`r` is the decimal-string revision, `a` is the address, and `v` is the new
value (boolean for X/Y/M, integer for D). This is the only change payload;
`source` and `oldValue` are not sent. There is no `format` parameter.
`dropped` and keep-alive records are unchanged.

The range stays within one X/Y/M/D area and can cover the entire configured
area; the read/write transaction limit does not apply. Addresses use the same
canonical syntax as read/write requests. Invalid, incomplete, or duplicate
query parameters return HTTP 400. The request URI length limits how many
explicit addresses fit in one URL. The server's `dropped` count covers the
global event ring, so a filtered client should re-read its subscribed values
when it receives `dropped`, even if some lost events were unrelated. The
stream does not send an initial snapshot; read the values separately.
