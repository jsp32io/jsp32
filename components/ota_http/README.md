# OTA HTTP API

The OTA API writes only the inactive application slot. It never updates the
LittleFS partition, so `/main.js` and all other filesystem data survive an
application update.

- `GET /api/v1/ota`
- `PUT /api/v1/ota/firmware`
- `POST /api/v1/ota/reboot`
- `POST /api/v1/ota/confirm`
- `POST /api/v1/ota/rollback`

`reboot` returns `202 Accepted` and restarts the device whether or not an
update is staged. It rejects a reboot while an OTA upload is in progress.

Firmware uploads use a raw `application/octet-stream` body with a required
`Content-Length` and `X-Content-SHA256` header. The image must fit the inactive
3 MB OTA slot, pass ESP-IDF image validation, and have the project name
`jsp32`. The upload buffers one 64 KiB physical erase block before each
sequential erase and write. The final block is padded with `0xff`; the SHA-256
header covers only the unpadded image. A successful upload selects it for the
next boot but does not reboot.

## Upload progress

An optional `X-Upload-ID` request header contains 32 lowercase hexadecimal digits.
During an active upload, `GET /api/v1/ota` returns:

```json
{
  "state": "receiving",
  "upload": {
    "id": "0123456789abcdef0123456789abcdef",
    "received": 70000,
    "written": 65536,
    "total": 100000,
    "percent": 70,
    "writtenPercent": 65.536
  }
}
```

`received`/`percent` retain their existing receive/buffer semantics. `written`
advances only after a successful `esp_ota_write`, once per 64 KiB block. It does
not count pending RAM data or the final block's `0xff` padding. `writtenPercent`
is the written image bytes divided by `total`. The counters may pause while a
block is erased/programmed; no interpolated erase percentage is reported.

After the last block is written, `state` becomes `verifying`. Writing all bytes
does not imply SHA-256, image validation or boot selection succeeded. Only the
successful PUT response with `state: ready` confirms completion. The active
`upload` object becomes null on completion or abort. The upload ID ties polling
to one request; clients must ignore snapshots for another ID. Without the
optional header, the ID is an empty string and existing upload clients continue
to work.

`cli ota upload` polls every 500 ms and displays matching device-written counts,
then verification, and completion only after PUT succeeds. Failed status probes
do not abort the upload. On older firmware without these fields, it waits for
the final result without inventing a percentage from local network buffering.

```sh
sha256sum build/jsp32.bin
curl -X PUT http://DEVICE_IP/api/v1/ota/firmware \
  -H 'Content-Type: application/octet-stream' \
  -H 'X-Content-SHA256: HEX_FROM_SHA256SUM' \
  --data-binary @build/jsp32.bin
curl -X POST http://DEVICE_IP/api/v1/ota/reboot
```

With bootloader rollback enabled, the first boot of the uploaded firmware is
`pending_verify`. Test the new firmware and call `confirm` to mark it valid.
Rebooting before confirmation makes the bootloader abort it and return to the
previous valid image. `rollback` explicitly invalidates the running image and
reboots into the previous valid image. `confirm` is idempotent. A staged update
must be booted before `rollback` is accepted; the endpoint never ambiguously
uses rollback as a way to cancel an update that has not booted yet.

Because this partition table has no factory app, the OTA component creates an
initial valid `otadata` entry for the serial-flashed `ota_0` image when the
table is still erased. This makes rollback available on the very first OTA,
not only after the second update.

On the tested S25FL128S1 board, the two `otadata` sectors are at `0x9000`
and `0xa000`, where 4 KiB erase was verified. A 4 KiB erase at `0x10000` did
not clear data. The application slots use 64 KiB block erase. The
partition layout change requires one serial deployment of the new partition
table, bootloader, and application before OTA can be used. An application-only
flash or OTA upload cannot migrate the old partition table.

The API currently has no authentication and must only be exposed on a trusted
network until the common HTTP authorization layer is added. SHA-256 detects
transfer corruption; it does not authenticate firmware. Secure Boot must be
enabled separately when signed firmware authenticity is required.
