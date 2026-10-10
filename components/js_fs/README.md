# JavaScript Filesystem API

`js_fs` installs the global `fs` object before `/main.js` runs. Paths are
absolute paths in the mounted LittleFS volume, such as `/plugins/logger/index.js`.
It uses the same filesystem implementation as the HTTP file API.

```js
fs.mkdir("/data");
fs.write("/data/readme.txt", "hello\n");
console.log(fs.readText("/data/readme.txt"));
const data = fs.readBytes("/data/readme.txt"); // ArrayBuffer
const info = fs.stat("/data/readme.txt");      // path, size, directory, sha256

let after;
do {
  const page = fs.list("/plugins", after);
  for (const entry of page.entries) console.log(entry.path, entry.directory);
  after = page.nextAfter;
} while (after !== null);

fs.remove("/data/readme.txt");
```

| Method | Result | Notes |
| --- | --- | --- |
| `fs.list(path = "/", after?)` | `{entries, nextAfter}` | Sorted direct children, at most 32 per page; pass `nextAfter` to fetch the next page. Each entry has `path`, `size`, and `directory`. |
| `fs.stat(path)` | `{path, size, directory: false, sha256}` | Regular files only. |
| `fs.readText(path)` | string | Reads one UTF-8 text file. |
| `fs.readBytes(path)` | `ArrayBuffer` | Reads one binary file. |
| `fs.write(path, data)` | `undefined` | Accepts a string, `Uint8Array`, or `ArrayBuffer`; atomically creates or replaces a file. Parent directory must exist. |
| `fs.mkdir(path)` | `undefined` | Creates one directory; parent must exist. |
| `fs.remove(path)` | `undefined` | Removes a file or empty directory. |

`/main.js`, `/plugins`, `/stdlib`, and everything below the latter two
directories are read-only through `fs`: `write`, `mkdir`, and `remove` throw
for these paths. Reading, listing, and stat remain available. The HTTP file
API retains its existing write behavior for deployment.

Reads and writes are limited to 64 KiB per call. These methods are synchronous
and execute on the QuickJS EventLoop task; large files or Flash operations can
delay timers and event callbacks. The underlying filesystem serializes access
with HTTP file operations. Changes to loaded ES modules take effect after a
restart, because QuickJS caches module imports.

Paths are checked by `app_filesystem`: no relative paths, `.`/`..` segments,
internal transaction paths, backslashes, or control characters. Errors throw
JavaScript exceptions.
