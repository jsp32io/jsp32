# Filesystem HTTP API

`fs_http` exposes the mounted application filesystem without exposing its VFS
mount point. Paths such as `/main.js` and `/settings/config.json` are relative
to the LittleFS root. Paths cannot contain `.` or `..` components.

- `GET /api/v1/fs/info`
- `GET /api/v1/fs/files?limit=50&after=%2Fmain.js`
- `GET /api/v1/fs/files?path=%2Fsettings` lists one directory
- `POST /api/v1/fs/directory?path=%2Fsettings` creates one directory
- `GET|HEAD /api/v1/fs/file?path=%2Fmain.js`
- `PUT /api/v1/fs/file?path=%2Fmain.js`
- `DELETE /api/v1/fs/file?path=%2Fname` removes a file or empty directory

File listings include a `directory` boolean. A non-empty directory returns
`FS_DIRECTORY_NOT_EMPTY` on deletion; clients must delete its contents first.

PUT bodies contain raw file bytes. `If-None-Match: *` makes the operation
create-only, while `If-Match: "sha256-HEX"` performs an optimistic conditional
replacement. `X-Content-SHA256: HEX` optionally verifies the uploaded bytes.
Reads return an ETag in the same format.

```sh
curl http://DEVICE_IP/api/v1/fs/files
curl 'http://DEVICE_IP/api/v1/fs/file?path=%2Fmain.js'

curl -X PUT 'http://DEVICE_IP/api/v1/fs/file?path=%2Fconfig.json' \
  -H 'Content-Type: application/json' \
  -H 'If-None-Match: *' \
  --data-binary @config.json

curl -X DELETE 'http://DEVICE_IP/api/v1/fs/file?path=%2Fconfig.json' \
  -H 'If-Match: "sha256-HEX_FROM_ETAG"'
```

File downloads are chunked and include `ETag`, `X-File-Size`, a no-store cache
policy, and `X-Content-Type-Options: nosniff`. JSON errors consistently
contain `ok: false` plus an error `code`, `name`, and `message`.

Writes are staged and committed through a recoverable LittleFS transaction. A
change to `/main.js` takes effect after restart because the running JavaScript
runtime already owns its in-memory entry source. Deleting `/main.js` is
disabled by default.

The API currently has no authentication and must only be exposed on a trusted
network until the common HTTP authorization layer is added.
