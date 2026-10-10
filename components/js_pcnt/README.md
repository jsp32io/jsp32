# JavaScript PCNT API

`js_pcnt` exposes the ESP32-S3 pulse counter hardware as a global `pcnt` object.
Pulses accumulate in hardware; JavaScript runs only when it calls a method.

```js
const pulses = pcnt.open({pin: 4, edge: "rising", pull: "up"});
pulses.start();
const first = pulses.sample(); // {count, timestampUs}
// Later: frequency in Hz = (second.count - first.count) * 1e6 /
//                         (second.timestampUs - first.timestampUs)
pulses.stop();
pulses.close();

const encoder = pcnt.openQuadrature({a: 4, b: 5}); // resolution: 4 by default
encoder.start();
console.log(encoder.read()); // signed position; direction changes the sign
encoder.close();
```

`pcnt.open({pin, edge?, pull?, filterNs?})` counts rising, falling, or both
edges. Defaults: `edge: "rising"`, `pull: "none"`, `filterNs: 0`.
`pcnt.openQuadrature({a, b, resolution?, pull?, filterNs?})` uses two distinct
pins. `resolution` may be `1`, `2`, or `4`, and defaults to `4` transitions per
complete A/B signal cycle. Reversing A and B reverses the reported direction.
`pull` applies to both pins in quadrature mode.

Both factories return a stopped counter with `start()`, `stop()`, `read()`,
`clear()`, `sample()`, and `close()`. Start and stop are idempotent. `stop()`
retains the count, `clear()` resets it, and `close()` is idempotent. Other
methods throw after close. `read()` returns a signed 32-bit integer. `sample()`
returns that count and a monotonic microsecond timestamp taken just after the
read; the pair is not a simultaneous hardware latch. A delta that spans signed
32-bit overflow is not defined.

Pins must be valid, allowed by firmware, and unreserved. Open counters reserve
their pins against GPIO, ADC, I²C, and UART. `close()` and runtime shutdown
release all resources. The driver allocates PCNT units from its resource pool;
opening beyond the available hardware fails.

The binding enables the ESP-IDF accumulation option and high/low limit watch
points to extend the hardware's 16-bit counter. It uses the widest signed
16-bit limits to reduce compensation interrupts. At very high rates, repeated
limit crossings can outpace the driver's compensation interrupt, so this API
does not claim a universal lossless input frequency. `filterNs` enables the
PCNT hardware glitch filter, whose duration is measured in nanoseconds; it is
not a mechanical switch debounce setting. A nonzero filter value may be
rejected by the driver if outside the hardware-supported range.

Implementation reference: [ESP-IDF PCNT driver](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/pcnt.html).
