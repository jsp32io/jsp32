# Application filesystem

This component mounts the `storage` LittleFS partition at `/storage` and owns all
path validation and file synchronization. Its public paths are logical root
paths such as `/main.js`; physical VFS paths are never accepted. The current
application API accepts root-level files only, so nested paths are rejected.

Reads, bounded sorted listing, SHA-256 metadata, conditional writes and deletes
are available through the C API. Writes use one internal staging file, backup,
and transaction log. Mount recovery either finishes or rolls back an interrupted
replacement, preventing a partially uploaded entry script from becoming
visible. Internal `/.fs-*` names are reserved and hidden. Mount failures do not
automatically format the partition, so invalid storage cannot silently erase
user data.

The build creates and flashes the initial LittleFS image from the project's `rootfs`
directory. Consequently, `rootfs/main.js` is available as `/main.js` in the
filesystem and `/storage/main.js` through ESP-IDF's VFS.

The local LittleFS component uses 64 KiB erase blocks for the S25FL128S main
array. Its image generator uses the same block size. A mount failure does not
format storage automatically.
