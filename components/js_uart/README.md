# JavaScript UART API

`js_uart` installs a global `uart` object through the `js_runtime` external
module interface:

```js
console.log(uart.list());

const port = uart.open(1, {tx: 17, rx: 18, baudRate: 115200});
await port.write("AT\r\n");

const data = await port.read(256, 1000);
if (data.length === 0) console.log("timeout");

port.close();
```

## Ports and configuration

`uart.list()` returns all SoC UART controllers with `id`, `available`, and
`open`. An unavailable controller also has a `reason`: `not-allowed`,
`console`, or `reserved`.

`uart.open(id, options)` accepts these fields:

| Field | Values | Default |
| --- | --- | --- |
| `tx` | valid allowed GPIO, or omitted | omitted |
| `rx` | valid allowed GPIO, or omitted | omitted |
| `baudRate` | integer `300-5000000` | `115200` |
| `dataBits` | `5`, `6`, `7`, `8` | `8` |
| `stopBits` | `1`, `1.5`, `2` | `1` |
| `parity` | `"none"`, `"even"`, `"odd"` | `"none"` |
| `rxBufferSize` | integer `256-16384` | menuconfig default |
| `txBufferSize` | integer `256-16384` | menuconfig default |
| `framing` | frame rule object (below) | omitted; byte-stream mode |
| `halfDuplex` | `{directionPin: gpio}` | omitted |

At least one of `tx` and `rx` is required. Unknown fields are rejected. A
port cannot be opened twice. Pins must be SoC-valid, included in the firmware
allowlist, and not reserved by GPIO or another driver.

`halfDuplex.directionPin` is the UART RTS output for a transceiver's active-high
DE and active-low /RE inputs (for example MAX3485, with DE and /RE tied
together). The ESP-IDF UART driver raises RTS while transmitting and lowers it
after the last bit. The JS protocol library only sees bytes; TTL, RS-232 and
RS-485 use the same `readFrame()` and `write()` calls.

## Framed input

Configure one frame rule when opening the port. `port.readFrame(timeoutMs)`
then resolves to one complete `Uint8Array`. A timeout before any input returns
an empty array; a partial frame at the deadline rejects the Promise. The
`timeoutMs` argument is the overall wait deadline, not a framing rule.

```js
const port = uart.open(1, {
  tx: 17, rx: 18, baudRate: 9600, dataBits: 8, parity: "even",
  framing: {type: "idle", gapChars: 3.5, maxBytes: 256},
  halfDuplex: {directionPin: 16},
});
const request = await port.readFrame(1000);
```

| Type | Rule fields | Completion |
| --- | --- | --- |
| `idle` | `gapChars` (default `4`, range `1-126`) | RX silence for the requested number of symbol times |
| `fixed` | `length` (required) | Exactly `length` bytes |
| `delimiter` | `byte` (required, `0-255`) | Delimiter byte received; returned frame includes it |
| `lengthField` | `offset` (default `0`), `width` (`1` or `2`, default `1`), `endian` (`"big"` or `"little"`, default `"little"`), `adjustment` (default `0`) | Total bytes = encoded length + adjustment |

All modes accept `maxBytes` (default `256`, at most the configured maximum
transfer size). An oversized frame rejects and clears the RX buffer to
resynchronize. `read()` cannot be mixed with `readFrame()` on a framed port.
For idle framing, hardware measures whole UART symbol periods, so a fractional
`gapChars` value is rounded up (for example `3.5` becomes `4`). This is a
frame-end detector; protocols with a separate *maximum inter-byte gap* must
validate that rule at the protocol layer or use stricter timing support.

## Asynchronous I/O

`port.write(data)` accepts a UTF-8 string, `Uint8Array`, or `ArrayBuffer` and
returns a Promise for the transmitted byte count. The Promise resolves only
after the hardware has finished transmitting.

`port.read(maxBytes, timeoutMs)` returns a Promise for a `Uint8Array`. It
resolves as soon as at least one byte is available, up to `maxBytes`. A zero
timeout only consumes buffered data; a timeout resolves to an empty array.
All reads and writes on one port execute in call order. A long pending read
therefore delays later writes; use a finite read timeout when the same port is
used bidirectionally.

Each port has a bounded operation queue. Validation errors throw
synchronously; driver failures and a full operation queue return rejected
Promises. The maximum transfer size and pending operation count are
configurable under **JavaScript UART** in menuconfig.

## Closing

`port.close()` is idempotent. It stops the native worker, rejects unfinished
operations with `UART closed`, calls `uart_driver_delete()`, releases its pins,
and invalidates the handle. The controller may then be reopened. Runtime
shutdown closes every open controller automatically.
