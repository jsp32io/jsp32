# NVS HTTP API

`nvs_http` exposes typed access to the default NVS partition. The API shows
`user`, `user_*`, and `jsp32_config` namespaces as writable. `jsp32_config`
retains the `managed` policy label and supports PUT, DELETE, and batch operations.
Its `ap_pass` and `wifi_pass` entries are writable but remain non-readable
through GET. Network settings use separate `wifi_mode` (u8), `ap_ssid`, `ap_pass`,
`wifi_ssid` and `wifi_pass` (string) keys. Use the built-in UI or
`/api/v1/network/config` to test and immediately apply network settings.
All other firmware and system namespaces are hidden. Writes update persisted
values; reboot to apply configuration loaded by firmware at startup.

## Routes

- `GET /api/v1/nvs`
- `GET /api/v1/nvs/namespaces?prefix=user_&limit=50&after=user_a`
- `GET|DELETE /api/v1/nvs/namespaces/{namespace}`
- `GET /api/v1/nvs/namespaces/{namespace}/entries?prefix=config.&limit=50&after=config.key`
- `GET|PUT|DELETE /api/v1/nvs/namespaces/{namespace}/entries/{key}`
- `POST /api/v1/nvs/namespaces/{namespace}/batch`

Namespace and key path segments are percent-decoded once. Names are limited
to the NVS limits and may contain letters, digits, `_`, `-`, and `.`.
List results are filtered by `prefix` first, then ordered and paginated using
the exclusive `after` cursor. NVS has no prefix index, so filtering scans the
entry metadata without reading values.

## Typed values and conditions

PUT uses an explicit type:

```json
{"type":"u32","value":42}
```

Supported types are `i8`, `u8`, `i16`, `u16`, `i32`, `u32`, `i64`, `u64`,
`float`, `double`, `string`, and `blob`. Because JSON cannot preserve every
64-bit integer, `i64` and `u64` values are decimal strings. Blobs use a base64
string and responses include `"encoding":"base64"`.

Entry reads and writes return a strong `ETag` in the form
`"sha256-HEX"`. `If-None-Match: *` makes PUT create-only; `If-Match` makes a
PUT or DELETE conditional on the current value.

```sh
curl -X PUT http://DEVICE_IP/api/v1/nvs/namespaces/user/entries/boot_count \
  -H 'Content-Type: application/json' \
  -H 'If-None-Match: *' \
  -d '{"type":"u32","value":1}'

curl http://DEVICE_IP/api/v1/nvs/namespaces/user/entries/boot_count
```

Deleting a whole namespace requires an exact confirmation header:

```sh
curl -X DELETE http://DEVICE_IP/api/v1/nvs/namespaces/user_cache \
  -H 'X-Confirm-Namespace: user_cache'
```

## Atomic batch

All operations in a batch are validated before changes are staged and share
one `nvs_commit()`. Duplicate keys are rejected. A failed condition or write
leaves the batch uncommitted.

```json
{
  "operations": [
    {"op":"set","key":"enabled","type":"u8","value":1},
    {"op":"set","key":"serial","type":"u64","value":"9007199254740993","ifNoneMatch":true},
    {"op":"delete","key":"obsolete","ifMatch":"\"sha256-HEX\""}
  ]
}
```

Body, string, blob, batch, worker-stack, and concurrency limits are
configurable under **NVS HTTP API** in menuconfig. The API uses the shared
HTTP asynchronous worker pool. It currently has no authentication and must
only be exposed on a trusted network until the common authorization layer is
added.
