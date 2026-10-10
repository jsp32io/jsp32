# Native MQTT telemetry

`app_mqtt` runs independently of JavaScript, using ESP-MQTT with one configured
broker connection. The plugin loader registers declarative telemetry through
`js_telemetry`; one native register subscription marks change reports, and a
native task takes register snapshots, decodes values, encodes JSON, and enqueues
messages. The client owns a bounded outbox and reconnects automatically.

Configuration is stored under `jsp32_config`. MQTT defaults to disabled.
The built-in `/mqtt` UI edits configuration and shows current connection/report
status; configuration changes require a device reboot. See the
[telemetry manual](../../man/js/telemetry.md) for declarations, NVS keys, HTTP
endpoints, types, payloads, and delivery semantics.

`mqtt_report.c` contains the declaration parser, snapshot encoder and report
scheduler. The service owns registered reports under its mutex. MQTT event
callbacks update atomic connection state only; they never enter QuickJS.
The native register callback copies no JS data and does not retain event memory.
The service/store live for the lifetime of the firmware; a JS runtime teardown
clears reports without stopping the broker connection.

Validation:

```sh
python3 tests/mqtt_host/run.py
node --test tests/plugins.test.mjs tests/app_ui_host/mqtt.test.cjs tests/app_ui_host/navigation.test.cjs
```

The host service test uses transport adapters to test queue failures and
reconnections deterministically. Actual ESP-MQTT/TLS transport is linked by the
firmware build and requires device/broker integration testing.
