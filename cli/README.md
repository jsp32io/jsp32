# jsp32 CLI

The CLI supports native Go serial flashing and the versioned HTTP API exposed
by a JSP32 device. Build with Go 1.25 or later. Set the
device URL through `JSP32_BASE_URL`; when its path does not end in an explicit
`/api/vN` version, `/api/v1` is appended automatically.

```sh
go build -o jsp32 .

./jsp32 flash -p /dev/ttyUSB0 ../build/jsp32.bin
./jsp32 flash -p /dev/ttyUSB0 ../build/factory.bin

JSP32_BASE_URL=http://192.168.1.20 ./jsp32 info
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 eval "1 + 1"
JSP32_BASE_URL=http://192.168.1.20/api/v1 ./jsp32 eval "console.log('1')"
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 man js
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 man js.gpio
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 console
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 fs ls
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 fs get /main.js >main.js
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 fs put /main.js main.js
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 fs mkdir /settings
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 fs ls /settings
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 fs rm -r /settings
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reg info
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reg get X0 M10 D100
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reg read D100 8 --json
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reg set M10=1 D100=42
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reg write D100 10 20 30
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reg watch M10 D100
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 nvs namespaces
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 nvs set user boot_count u32 1
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 nvs get user boot_count
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 ota status
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 ota upload ../build/jsp32.bin
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reboot -wait
```

`flash -p PORT firmware.bin` burns firmware over serial without Python or a
device URL. Both `.bin` and `.bin.gz` are supported; gzip is detected by its
header and decompressed in memory before image identification. Gzip checksum
errors, truncated streams, and images larger than 16 MiB after decompression
are rejected before connecting. Reported image sizes and alignment refer to the
uncompressed firmware. For example:

```sh
./jsp32 flash -p /dev/ttyUSB0 ../build/jsp32.bin.gz
./jsp32 flash -p /dev/ttyUSB0 ../build/factory.bin.gz
```

It validates the ESP32-S3 image headers, segment checksums, image
SHA-256 and factory partition-table MD5 before connecting. Classification uses
image contents, so renaming a file does not change its destination. A merged
factory image (including initial storage) is written at `0x0`. For an application
image, the CLI reads the device's partition table and both `otadata` records,
selects the opposite OTA slot, and reports its address. If `ota_0` is selected,
it writes `ota_1` (normally `0x320000`); if `ota_1` is selected, it writes `ota_0`
(normally `0x20000`). Only after the image MD5 passes does it update and verify
the other boot record to select the new slot on the next boot. The previous boot
record is preserved. Serial-flashed firmware is marked VALID after successful
verification and requires no `jsp32 ota confirm`. It does not undergo the
bootloader's first-boot confirmation/automatic rollback cycle. An existing
PENDING_VERIFY selection can also be replaced through serial flashing.
HTTP `ota upload` continues to use the confirmation and rollback workflow.

Application updates require the project's two-slot layout without a factory
application partition. Erased OTA records imply `ota_0`; after verifying the
new image, the CLI seeds a confirmed `ota_0` fallback record before activating
`ota_1`. Unusable non-erased records cause an error instead of guessing a slot.
OTA metadata is written in separate 4 KiB sectors, independent of `-align`;
the Flash must support 4 KiB erases at the metadata addresses. Initial deployment
requires a factory image. Factory flashing
replaces the configuration and filesystem regions included in the image. A
trimmed factory image leaves the omitted tail untouched and requires blank or
previously erased Flash for a clean initial deployment.

Connection status, live transfer progress and verification status go to standard
error; the success result goes to standard output only after an explicit device
MD5 check passes. Compressed-transfer percentages estimate image progress from
acknowledged compressed blocks; 100% transfer precedes final verification.
Images are padded with `0xFF` to the alignment selected by `-align SIZE`,
defaulting to `64K` for S25FL128S erase geometry. Sizes accept bytes, hexadecimal,
or binary K/M suffixes: `4096`, `0x10000`, `4K`, `64KiB`, `128K`. The alignment
must be a power of two from 4 KiB to 16 MiB. The destination address must also
meet this alignment, and the padded image must fit its partition and Flash.
For example, use `-align 4K` for a Flash supported by the stub with 4 KiB erases.
This option controls padding and address validation; it does not change the
Flash chip's erase geometry or the stub's erase commands.
The default transfer baud rate is 115200; `-b 460800` selects a higher rate when
the serial transceiver and connection support it.

`-reset default` uses DTR/RTS with the board's automatic download circuit. For
UART0 through MAX3232 with only TX/RX, enter download mode using BOOT/RESET and
use `-reset no-reset`; press RESET again after successful flashing. For native
USB Serial/JTAG use `-reset usb-jtag`. `-reset auto` tries the available reset
strategies. Close serial monitors before flashing. Interrupting the command
closes the serial connection and stops the operation.

```sh
./jsp32 flash -p /dev/ttyUSB0 -reset no-reset ../build/jsp32.bin
./jsp32 flash -p /dev/ttyACM0 -reset usb-jtag ../build/factory.bin
```

`info` queries `GET /api/v1/system/info` and prints the complete response as
indented JSON, including board/chip, firmware version, uptime/reset reason,
Flash/PSRAM, MAC/IP addresses, heap usage and partitions. The output preserves
additional fields from newer firmware and can be piped to `jq`.
`network.station.ssid` and `network.station.rssi` report the connected Wi-Fi
network and its signal strength in dBm, queried on each request. Both are null
when the station is disconnected or the signal cannot be read.

`man [topic]` fetches the device's embedded manual and writes the original Markdown
without formatting or an extra newline. `man` defaults to the root `index.md` (topic `index`); `man js`
reads the JS directory's `index.md`, and `man js.gpio` reads `js/gpio.md` through
`GET /api/v1/man/js.gpio`. Unknown topics return an error and a nonzero exit code.
Use `man --help` for usage. Output can be redirected to a file or piped to a pager:

```sh
./jsp32 man js.gpio > gpio.md
./jsp32 man js | less
```

`eval` prints the JavaScript return value. It does not subscribe to console
events, so the second example prints `undefined`, not the value passed to
`console.log`.

`console` streams recent and live `console.log` output until interrupted. If
the device reports that a slow client missed messages, the CLI writes a
warning to standard error.

`fs` manages files in the device's logical filesystem. Run `jsp32 fs help` for
the full command list. The optional local path for `get` and `put` defaults to
`-` (standard output or input). Downloads written to a local path are replaced
atomically; uploads include a content SHA-256, and `--create` or
`--if-match SHA256` can prevent accidental overwrites. Updating `/main.js`
requires a device restart before the new script is used.
`fs mkdir` creates one directory. `fs rm` removes files and empty directories;
use `fs rm -r` to remove a non-empty directory and its contents. `fs ls` accepts
an optional directory path and marks directories with `dir`.

`reg` reads and writes the native RAM register table. Addresses are uppercase
`X`, `Y`, `M`, or `D` followed by a zero-based index. `X` is read-only;
`X/Y/M` values are bits (`0`, `1`, `true`, or `false`); `D` values are unsigned
16-bit words (decimal or `0x` hexadecimal). A `set` or `write` invocation is
one atomic transaction, limited by `reg info`'s `max-transaction`. Values are
volatile and reset on reboot. `watch` streams changes until interrupted;
optional addresses select a server-side subscription. Normal `watch` output
has three columns: revision, address, and new value. `get`, `read`, and `watch`
support `--json` for machine-readable output; `watch --json` emits the compact
SSE payload with keys `r`, `a`, and `v`.

`nvs` supports reading and modifying `user`, `user_*`, and `jsp32_config`
namespaces. Network settings use independent keys in `jsp32_config`:
`wifi_mode` (u8: 1=AP, 2=STA, 3=APSTA), `ap_ssid`, `ap_pass`, `wifi_ssid`,
and `wifi_pass` (strings). AP addressing uses separate string keys `ap_ip`
and `ap_netmask` (defaults: `192.168.7.1` and `255.255.255.0`). The gateway equals
the AP address. Missing address keys use defaults independently of `wifi_mode`;
invalid addresses or masks fall back to the default address and mask. `ap_pass` and `wifi_pass` are writable but remain
non-readable through `nvs get`. Use the built-in UI or `/api/v1/network/config`
for immediate changes with connection testing; direct NVS writes take effect
on reboot. Removing `wifi_mode` returns to anonymous AP defaults on the next boot.
`rm`, `batch`, and `clear --yes` also support `jsp32_config`. This requires firmware
with writable `jsp32_config` support.

For example, configure AP addressing and reboot:

```sh
./jsp32 nvs set jsp32_config ap_ip string 192.168.7.1
./jsp32 nvs set jsp32_config ap_netmask string 255.255.255.0
./jsp32 reboot
```

AP addresses must be unicast host addresses, and masks must be contiguous
(`/1` through `/30`); network and broadcast addresses are rejected at boot.
Captive DNS, redirects, and network-page reconnect hints follow the active AP IP.

`nvs` provides typed access to visible NVS namespaces. Integer widths, strings,
floating-point values, and blobs are preserved; 64-bit integers are encoded as
decimal strings to avoid JSON precision loss. `nvs get` prints typed JSON and
`--raw` prints only the decoded value. For `nvs set ... blob`, the value argument
is a local file or `-` for standard input. Conditional writes use `--create` or
`--if-match SHA256`, and `nvs batch` accepts an atomic operation document from a
file or standard input.

`ota` stages a validated application image in the inactive OTA slot without
touching LittleFS. Upload progress on standard error uses the device's completed
Flash writes, polled every 500 ms and correlated by a unique upload ID. Progress
is updated in place using backspaces, clearing any leftover text; it ends with a
newline on success or failure. It does
not count bytes merely read by the local HTTP transport. Each completed 64 KiB
erase/write advances progress; final padding is excluded. After writing, the
CLI shows verification and only reports completion when the upload response is
successful. Older firmware without write counters shows a waiting message;
status probe failures do not cancel an otherwise valid upload. The final result
remains on standard output. Upload and reboot are deliberately separate
commands. Top-level `jsp32 reboot` also restarts the device when no update is staged.
Add `-wait` to return only after a new boot responds to the system info API;
each status request has a 500 ms timeout, with 200 ms between checks.
The overall wait times out after 30 seconds.
After booting an update, test it and run `jsp32 ota confirm`;
rebooting an unconfirmed image invokes the bootloader rollback policy. `ota
rollback --yes` explicitly invalidates the running image and reboots to the
previous valid slot.
