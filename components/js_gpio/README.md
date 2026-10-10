# JavaScript GPIO API

`js_gpio` installs a global `gpio` object through the `js_runtime` external
module interface:

```js
gpio.list();

gpio.setup(2, {mode: "output", initial: 0});
gpio.write(2, 1);
console.log(gpio.read(2));

gpio.setup(4, {mode: "input", pull: "up", debounceMs: 30});
const unwatch = gpio.watch(4, "falling", event => {
  console.log(event.pin, event.value, event.edge);
});
unwatch();

gpio.reset(4);
```

## Pins

`gpio.list()` returns every valid ESP32-S3 GPIO (`0-21` and `26-48`) with its
live state:

```js
[{pin: 0, available: true, configured: false, mode: null},
 {pin: 19, available: false, configured: false, mode: null,
  reason: "reserved"}]
```

An unavailable pin has reason `reserved` or `not-allowed`. The allowed-pin
mask is supplied by native firmware. SoC-invalid pins and pins reserved by
Flash, PSRAM, USB, or another driver cannot be configured.

## Setup and I/O

These are all supported setup fields:

| Field | Mode | Values | Default |
| --- | --- | --- | --- |
| `mode` | both | `"input"`, `"output"` | required |
| `pull` | input | `"none"`, `"up"`, `"down"` | `"none"` |
| `initial` | output | `0`, `1`, `false`, `true` | `0` |
| `debounceMs` | input | integer `0-60000` | `0` |

Unknown fields and fields used with the wrong mode are rejected. Reads return
`0` or `1`; writes accept `0`, `1`, or a boolean. The initial output latch is
set before output is enabled to avoid a configuration glitch.

Calling `setup` with the same normalized configuration is a no-op and keeps
watchers. A changed configuration removes all watchers before it is applied.
Omitted defaults participate in comparison, so `{mode: "input"}` equals
`{mode: "input", pull: "none", debounceMs: 0}`.

## Watchers

`watch` accepts `rising`, `falling`, or `both`. Every call creates an
independent subscription and returns an idempotent unsubscribe function. A
pin uses one native interrupt regardless of its watcher count. Debounce is a
stable-state delay shared by every watcher on that pin.

Callbacks receive `{pin, value, edge, timestampUs}` and run in registration order.
`timestampUs` is monotonic microseconds since boot, captured at the hardware
edge; a debounced event keeps the final edge's timestamp. A rising
edge calls `rising` and `both` subscriptions; a falling edge calls `falling`
and `both` subscriptions. Repeated registrations are independent. Dispatch
uses a listener snapshot, so adding, cancelling, reconfiguring, or resetting
inside a callback affects the next event rather than the current one.

`reset` and runtime shutdown remove watchers and restore pin defaults. GPIO
interrupts only enqueue fixed-size C events; JavaScript callbacks always run
in the runtime EventLoop task. The global watcher limit is configurable under
**JavaScript GPIO** in menuconfig and defaults to 32.
