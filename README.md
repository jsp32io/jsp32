# JSP32

English | [简体中文](README_zh.md)

JSP32 is JavaScript-programmable device firmware for the ESP32-S3. Use scripts to
read sensors, control digital I/O, and handle serial and network communication.
Registers, Modbus, and MQTT connect your device logic to other systems. The
firmware includes a web interface, with a CLI and a Windows / Linux desktop
console available for device management.

The default hardware configuration is ESP32-S3 N16R8: 16 MB Flash and 8 MB Octal PSRAM.

## Features

- **Device logic in JavaScript**: QuickJS supports ES modules, Promises, timers,
  and event listeners. Scripts live in the device filesystem, so updating your
  application logic does not require rebuilding the firmware.
  Asynchronous reads accept `{timeoutMs}` options: zero means unlimited waiting,
  and wait timeouts reject with `code: "TIMEOUT"`.
- **Plugins with configurable settings**: Plugins start in dependency order,
  declare settings, and register resource cleanup callbacks. View their status
  and edit configuration through the web interface or desktop console.
- **Hardware interfaces**: Digital GPIO with debounce, pulse and quadrature
  encoder counting, ADC sampling, asynchronous UART I/O and framing, and I²C
  device access.
- **Shared register data**: PLC-style X/Y/M/D registers are shared by scripts,
  the HTTP API, and Modbus. Atomic reads and writes and change subscriptions
  connect device logic to host applications.
- **Networking and telemetry**: TCP clients and listeners, TLS clients, a
  Modbus TCP server, a JavaScript Modbus RTU slave, and periodic or change-based
  MQTT reporting for plugin data.
- **Built-in device management**: The web interface provides system information,
  Wi-Fi configuration, live register browsing, and plugin and MQTT settings.
  APIs support local device discovery, logs, file management, remote script
  execution, and OTA updates.
- **Independent native services**: Wi-Fi management and HTTP services run
  independently of JavaScript. An EventLoop schedules script callbacks,
  Promises, and peripheral events, with execution budgets and resource limits.

## Using a device

A device without a saved network mode starts a `JSP32-XXXX` hotspot. Connect to it
and open `http://192.168.4.1/` in your browser. The network page supports hotspot
mode (AP), connection to an external Wi-Fi network (STA), or both (APSTA).
Network settings are saved on the device and restored on reboot.

| Interface | Use it for |
| --- | --- |
| Built-in web interface | Device information, network setup, live registers, plugin and MQTT configuration |
| [Desktop console](gui/README.md) | Local device discovery, register monitoring, plugin ZIP uploads and downloads, plugin configuration |
| [CLI](cli/README.md) | Script evaluation, live logs, files and registers, NVS management, serial flashing and OTA firmware updates |
| [HTTP API](components/app_http_server/README.md) | Integrating device management with your own tools and systems |

Web pages are available at `/`, `/network`, `/registers`, `/plugins`, and `/mqtt`.
The device advertises `_jsp32._tcp.local` through mDNS. The desktop console can
discover devices on the same local network or connect to an address entered
manually. Management interfaces have no authentication and should be used on a
trusted network.

## Writing device logic

On startup, the device executes `/main.js`. The plugin loader starts plugins
from `/plugins/<name>/index.js`. Plugins can use global hardware APIs and import
JavaScript libraries from `/stdlib`.

For example, this plugin logs a heartbeat every second:

```js
export default {
  setup({onCleanup}) {
    const timer = setInterval(() => console.log("alive"), 1000);
    onCleanup(() => clearInterval(timer));
  },
};
```

Save it on the device as `/plugins/heartbeat/index.js` and reboot to run it.
Deploy plugin files through the CLI file interface or desktop console; use the
web interface to view status and edit settings. See [plugin development and
deployment](man/js/plugins.md) for the full workflow and configuration declarations.

| Capability | Documentation |
| --- | --- |
| Script execution, modules, logs, timers, and events | [JavaScript API index](man/js/index.md) |
| GPIO, PCNT, ADC, UART, and I²C | [Peripheral APIs](man/js/index.md#外设与寄存器) |
| X/Y/M/D registers | [Registers](man/js/registers.md) |
| TCP and TLS | [Networking](man/js/net.md) |
| MQTT telemetry fields and reporting rules | [Telemetry](man/js/telemetry.md) |
| Modbus RTU / TCP | [RTU slave](man/js/modbus.md) · [TCP server](components/modbus_tcp/README.md) |
| Digital inputs and encoder-based length measurement | [io plugin](man/js/plugins/io.md) |
| TCP forwarding and remote access | [online plugin](man/js/plugins/online.md) |
| File management | [Filesystem](man/js/fs.md) |

The script environment does not provide browser `fetch`, desktop `std/os`
modules, or npm package resolution. Register values live in RAM and reset to
zero on reboot. Store persistent data in files or through the NVS management API.

## Project structure

```text
main/                  Firmware entry point; assembles native services and JS modules
components/            Native services, JavaScript bindings, and vendored dependencies
  js_runtime/          QuickJS EventLoop, timers, listeners, and async request management
  js_gpio/ ...         Script APIs for GPIO, PCNT, ADC, UART, I²C, networking, and more
  app_ui/              Web interface embedded in the firmware
  network_manager/     Wi-Fi modes, saved configuration, and connection recovery
  app_registers/       Native register data and transactions
  app_mqtt/            MQTT connections and telemetry reporting
rootfs/                LittleFS image contents
  main.js              JavaScript entry point
  plugins/             Device plugins
  stdlib/              JavaScript libraries and device drivers
  examples/            Script examples
man/                   Device manuals included in the firmware
cli/                   Go command-line client
gui/                   C++ desktop console (SDL3 / Dear ImGui)
tests/                 Host tests and integration tests
tools/                 Flashing, Flash erase, and manual embedding tools
sdkconfig.defaults     Default hardware and feature configuration
partitions.csv         Partition layout for 16 MB Flash
```

## Building and deploying

The firmware uses ESP-IDF v6.1.0 and the ESP32-S3 toolchain. Load the environment
and build from the project root:

```sh
. /path/to/esp-idf/export.sh
idf.py build
```

The application image is `build/jsp32.bin`; the filesystem image is
`build/storage.bin`. Dependencies such as QuickJS, cJSON, mDNS, and MQTT are
vendored in `components/`, so normal firmware builds do not need downloads from
the component registry. Defaults use QIO Flash at 40 MHz, Octal PSRAM at 80 MHz,
and the native USB Serial/JTAG console. Use `idf.py menuconfig` to edit an
existing `sdkconfig`; `sdkconfig.defaults` supplies the initial configuration.

For boards using S25FL128S1 Flash, use the included script to update the
application over serial:

```sh
python tools/s25fl_flash_app.py --port /dev/ttyACM0 --address 0x20000 build/jsp32.bin
idf.py -p /dev/ttyACM0 monitor
```

Replace the serial port with your device's port. This Flash uses 64 KiB erase
blocks in its main array; the script pads the application image before writing
to avoid tail-block erase issues with ordinary flashing tools. It updates only
the application. Initial deployment or a different partition layout requires
serial flashing of the matching bootloader, partition table, application, and
filesystem image; back up device data first. See the [S25FL128S1 component
notes](components/s25fl128s/README.md) for Flash geometry, frequency, and board-specific details.

The default partition layout provides two 3 MiB OTA application slots and
9.875 MiB of LittleFS storage. Application slots start at `0x20000` and
`0x320000`; the filesystem starts at `0x620000`. Normal OTA updates preserve the
filesystem. After the first boot of a new OTA image, confirm it through
`POST /api/v1/ota/confirm`. Resetting before confirmation rolls back to the
previous valid image. See the [CLI](cli/README.md) and [OTA API](components/ota_http/README.md)
for update instructions.

For 8 MB Flash, use `partitions_8mb.csv` and adjust both the Flash size and
partition table configuration. This layout provides 1.875 MiB of LittleFS storage.
Board wiring, available pins, and Flash settings must match the actual hardware.

## Documentation

- [Device manuals](man/index.md): User-facing APIs, plugins, and libraries, also
  available from the device through the CLI `man` command.
- [Web interface](components/app_ui/README.md), [desktop console](gui/README.md),
  and [CLI](cli/README.md): Management tools, usage, and build instructions.
- [Runtime component](components/js_runtime/README.md): Native module integration,
  EventLoop scheduling, and resource lifecycles.
- Component READMEs: Service configuration, HTTP interfaces, and native implementation details.

Detailed manuals and component documentation use their existing languages; some are in Chinese.
