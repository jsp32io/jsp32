# Application Registers

`app_registers` provides a volatile, thread-safe PLC-style register image.
There is no persistence layer in this version.

The address spaces are:

| Area | Element | Default size | JavaScript access |
| --- | --- | ---: | --- |
| `X` | input bit | 256 | read/write |
| `Y` | output bit | 256 | read/write |
| `M` | internal bit | 1024 | read/write |
| `D` | 16-bit word | 2048 | read/write |

Sizes and maximum transaction length are configured through Kconfig. Addresses
are uppercase canonical strings such as `X0`, `M42`, and `D100`; leading zeroes
are rejected.

All APIs are task-safe but are not ISR-safe. A single read or write is atomic.
Range reads observe one snapshot, and range or arbitrary batch writes validate
the complete request before changing memory. A successful transaction which
changes at least one value increments the global revision exactly once. Writing
only values already present is a no-op and preserves the revision.

`X` can be written by JavaScript, drivers, or the system, but not through HTTP.
Subscribers run synchronously after the storage
lock is released and receive only values changed by the transaction. A
subscriber must copy event data before returning if it needs to retain it.

The central APIs are declared in `include/app_registers.h`:

```c
app_registers_t *registers = NULL;
ESP_ERROR_CHECK(app_registers_init(&registers));

app_register_address_t start = {
    .area = APP_REGISTER_AREA_D,
    .index = 100,
};
uint16_t values[] = {10, 20, 30};
uint64_t revision = 0;
ESP_ERROR_CHECK(app_registers_write_words(
    registers, start, values, 3, APP_REGISTER_SOURCE_SYSTEM, &revision));
```
