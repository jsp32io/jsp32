# JavaScript TCP / TLS API

`js_net` 通过 Runtime 外部模块接口安装全局 `net` 和 `SocketError`。

TLS 通过同一个对象上的 `net.dialTLS()` 提供，返回相同的连接接口。
完整参数、证书验证和关闭语义见 [Net 手册](../../man/js/net.md)。
TLS 始终使用系统 CA；`ca: [pemCA1, pemCA2]` 为本次连接附加自定义 CA。

```js
const conn = await net.dialTLS({
  host: "gateway.example.com", port: 9443,
  ca: [companyRootCA, companyBackupCA], // 可省略，系统 CA 始终启用
});
try {
  await conn.write(new Uint8Array([1, 2, 3]));
  const data = await conn.read(4096, {timeoutMs: 3000});
} finally {
  await conn.close();
}
```

```js
const conn = await net.dialTCP({
  host: "192.168.1.100", port: 9000, timeoutMs: 5000,
});
try {
  await conn.write(new Uint8Array([1, 2, 3]));
  const data = await conn.read(4096, {timeoutMs: 3000});
  if (data === null) console.log("EOF");
} finally {
  await conn.close();
}
```

## Connect and listen

`net.dialTCP({host, port, timeoutMs?, noDelay?})` 返回 `Promise<TCPConn>`。
`host` 是 IP 地址或域名；`port` 为 `1–65535`；`noDelay` 默认 `true`。
域名使用系统 DNS，连接解析结果中的第一个 IPv4/IPv6 地址，不逐个重试。
DNS 在独立任务中执行，不阻塞其他连接。建连超时包含排队和 DNS 时间；
超时后的 DNS 结果会丢弃。系统 DNS 查询不能中途取消，因此 Runtime 清理
会等待已开始的查询结束。

`net.listenTCP({port, host?, backlog?, timeoutMs?})` 返回
`Promise<TCPListener>`。`host` 默认 `"0.0.0.0"`，必须是 IP 字面量；
`port` 为 `0–65535`，`0` 自动分配端口；`backlog` 默认 `4`，范围 `1–128`。

```js
const listener = await net.listenTCP({host: "0.0.0.0", port: 9000});
try {
  const conn = await listener.accept({timeoutMs: 30000});
  try {
    const data = await conn.read(4096);
    if (data !== null) await conn.write(data);
  } finally {
    await conn.close();
  }
} finally {
  await listener.close();
}
```

`accept()` 返回 `Promise<TCPConn>`。每个 listener 最多一个待决 accept；
重复调用 reject `BUSY`。关闭 listener 会取消 accept，已经接受的连接独立存活。

## Byte streams

| Method | Result |
| --- | --- |
| `conn.read(maxBytes, {timeoutMs?}?)` | `Promise<Uint8Array \| null>` |
| `conn.write(data, {timeoutMs?}?)` | `Promise<number>` |
| `conn.closeWrite({timeoutMs?}?)` | `Promise<void>` |
| `conn.close()` / `listener.close()` | `Promise<void>` |

`read()` 在收到至少一个字节时返回，最多 `maxBytes`；只有 EOF 返回 `null`。
一次读取不对应一个协议帧，协议库需要自行累计数据。每条连接最多一个待决
read，重复调用 reject `BUSY`。没有 read 请求时，不主动读取应用数据。

`write()` 接受 `ArrayBuffer` 或字节 typed array（包括 `Uint8Array`），
并在调用时复制当前视图中的字节。空写入返回 `0`。数据全部交给本地 TCP 栈
后返回字节数；这不表示对端应用已经收到。write 按调用顺序执行，read 和
write 独立推进。等待 write Promise 可控制生产速度；超过有界队列或原生
缓冲预算时 reject `QUEUE_FULL`。

`closeWrite()` 排在此前 write 后面发送 FIN，然后拒绝新的写操作；read
仍然可用。对端 FIN 不会自动关闭本端写侧，可以先读到 EOF 再发送响应。

`close()` 幂等，调用后立即拒绝新的 I/O，取消未完成操作，原生连接关闭且
待决操作完成处理后 resolve。它不等待待发送数据；需要有序结束发送时先
`await conn.closeWrite()`。Runtime 停止时关闭全部连接；GC 会请求关闭
无人引用且没有待决操作的句柄，但应使用显式 close 管理生命周期。

连接有只读 `localAddress`、`remoteAddress`，listener 有只读 `localAddress`，
值为 `{address, port, family: 4 | 6}`。两类句柄都有实时只读 `closed`。

## Timeouts and errors

默认 dialTCP/listenTCP/write/closeWrite 超时为 `5000` ms；dialTLS 为
`10000` ms；read/accept 默认无限等待。
`timeoutMs` 是从调用开始计算的整个操作期限，`0` 表示无限等待。
read/accept 超时不关闭句柄；write/closeWrite 超时关闭连接，取消后续操作，
避免部分发送的数据破坏协议状态。

参数错误同步抛出 `TypeError` / `RangeError`；异步和资源错误 reject
`SocketError`。未知 options 字段会被拒绝。错误有 `code`、`operation`、
可选 `errno`；write 错误还有 `bytesWritten`，TCP 表示失败前交给本地 TCP 栈
的字节数，TLS 表示成功提交的应用明文字节数，不是密文字节数。

`code`：`BUSY`、`CLOSED`、`TIMEOUT`、`QUEUE_FULL`、`RESOURCE_LIMIT`、
`INVALID_ADDRESS`、`DNS`、`CONNECTION_REFUSED`、`CONNECTION_RESET`、`IO`。

## Native integration and limits

```c
ESP_ERROR_CHECK(js_net_register()); /* before js_runtime_start() */
```

一个网络 worker 使用非阻塞 lwIP sockets 和 `select()`，一个 DNS worker
处理系统解析。只有 EventLoop 操作 QuickJS。结果存入预留容量的完成队列，
Runtime 事件仅用于唤醒；队列满导致的唤醒丢失会自动重试。网络 worker 的
等待时间上限为 20 ms，新请求、关闭和超时通常在下一轮推进。

menuconfig 的 **JavaScript network** 默认限制为 16 个连接/listener（可配置 1–64）、
32 个待决操作、单次传输 16 KiB、全部待决读写原生缓冲 64 KiB。
连接数包含 listener，底层 lwIP socket 配额与 HTTP/MQTT 等服务共享。
项目默认提供 32 个 lwIP sockets，使用默认的 `FD_SETSIZE=64`。
若将 socket 配额提高到 61 以上，需要同步扩大 `FD_SETSIZE`。
TLS 客户端由 `CONFIG_JS_NET_TLS_ENABLED` 启用，默认最多 2 个 TLS 上下文，
包含正在握手的连接。CA 字符串快照计入原生缓冲预算，解析后的证书和
mbedTLS 上下文另外占用原生堆；关闭及建连失败都会释放。UDP 不在此 API 范围内。

系统 bundle 验证回调的版本适配集中在 `socket_tls.c`，当前基于 ESP-IDF
自带的 mbedTLS 4 配置字段。升级 SDK 时应重跑下述真实 TLS 测试。

主机测试：`python3 tests/js_net_host/run.py`。测试使用真实 QuickJS、
ESP-IDF mbedTLS / bundle 验证代码、POSIX 回环 TCP/TLS 和 pthread 模拟
FreeRTOS；需要本地 ESP-IDF 源码（通过 `IDF_PATH` 指定）、CMake 和 Python
`cryptography`。可通过 `SANITIZE=address,undefined` 启用内存检查。
