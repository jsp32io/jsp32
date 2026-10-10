# Filesystem

[JavaScript API 索引](index.md)

`fs` 是全局 LittleFS 文件管理对象。路径从文件系统根目录开始，例如
`/plugins/logger/index.js`。方法同步执行，单次读写最多 64 KiB。

```js
fs.mkdir("/data");
fs.write("/data/config.json", JSON.stringify({enabled: true}));
const config = JSON.parse(fs.readText("/data/config.json"));

let after;
do {
  const page = fs.list("/plugins", after);
  for (const entry of page.entries) console.log(entry.path, entry.directory);
  after = page.nextAfter;
} while (after !== null);
```

| 方法 | 返回值 | 说明 |
| --- | --- | --- |
| `fs.list(path = "/", after?)` | `{entries, nextAfter}` | 每页最多 32 个按路径排序的直属条目；`nextAfter` 为 `null` 时结束。条目含 `path`、`size`、`directory`。 |
| `fs.stat(path)` | `{path, size, directory: false, sha256}` | 查询普通文件并计算 SHA-256。 |
| `fs.readText(path)` | string | 读取文本。 |
| `fs.readBytes(path)` | `ArrayBuffer` | 读取二进制内容。 |
| `fs.write(path, data)` | `undefined` | 原子创建或替换文件；支持字符串、`Uint8Array` 和 `ArrayBuffer`。 |
| `fs.mkdir(path)` | `undefined` | 创建一级目录，父目录须存在。 |
| `fs.remove(path)` | `undefined` | 删除文件或空目录。 |

`/main.js`、`/plugins`、`/stdlib` 及后两个目录下的所有路径，在 `fs` 中只读：
`write`、`mkdir`、`remove` 对这些路径抛异常；读取、列目录和查询文件信息仍可用。
HTTP 文件接口保持原有写入行为，可用于部署脚本和插件。

无效路径或文件系统错误会抛异常。路径必须为绝对路径，不能包含 `.`、`..`
路径段、反斜杠或控制字符。Flash 操作会占用 EventLoop；已导入的 JS 模块
在重启前不会因文件替换而重新加载。
