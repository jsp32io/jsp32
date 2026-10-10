# JavaScript HTTP API

`js_http` adapts transport-neutral `js_runtime` services to HTTP. It currently
provides:

- `GET /api/v1/quickjs/console/events`: an SSE stream of recent and live
  `console.log()` output.
- `event: dropped`: reports how many console lines a slow client missed.
- `POST /api/v1/quickjs/eval`: evaluates JavaScript on the runtime EventLoop
  and optionally waits for a returned Promise.

## Evaluation

The request body is JSON:

```json
{
  "source": "await new Promise(resolve => setTimeout(() => resolve(42), 100))",
  "mode": "expr",
  "await": true,
  "timeoutMs": 5000
}
```

`mode` defaults to `expr`, `await` defaults to `true`, and `timeoutMs` defaults
to 5000 ms. `expr` treats `source` as one expression and implicitly returns its
value from an async function. `script` evaluates `source` as an async global
script, so it supports statements, top-level `await`, and changes to global
state; its result follows QuickJS script completion semantics.

For example:

```sh
curl -X POST http://DEVICE_IP/api/v1/quickjs/eval \
  -H 'Content-Type: application/json' \
  -d '{"source":"await Promise.resolve(42)","mode":"expr"}'

curl -X POST http://DEVICE_IP/api/v1/quickjs/eval \
  -H 'Content-Type: application/json' \
  -d '{"source":"globalThis.answer = await Promise.resolve(42); answer","mode":"script"}'
```

A successful result includes `ok`, `async`, `type`, `value`, and a display
string when one is available. JavaScript exceptions are normal evaluation
results and use HTTP 200 with `ok: false`; overload, runtime unavailability,
and timeouts use HTTP 429, 503, and 504 respectively.

Evaluation is remote code execution inside the device's shared JavaScript
runtime. The endpoint currently has no authentication or authorization and
must only be exposed on a trusted network.

SSE and eval each have local limits and acquire a shared asynchronous worker
slot from `app_http_server`; filesystem transfers use the same global pool, and
at least one HTTP socket remains available for short requests. Body, source,
result, timeout, task, and concurrency limits are available under
**JavaScript HTTP API** in menuconfig. JavaScript is always evaluated by the
`js_runtime` EventLoop task; HTTP worker tasks never access QuickJS values.
