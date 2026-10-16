# Net API：TCP 与 TLS

[返回索引](index.md)

全局 `net` 采用 Promise 风格，连接通过 `read`、`write`、`closeWrite`、
`close` 操作。TCP 由 `js_net` 实现，启用配置为 `CONFIG_JS_NET_ENABLED`。

TCP 与 TLS 客户端均由 `js_net` 提供。TLS 启用配置为
`CONFIG_JS_NET_TLS_ENABLED`，依赖固件系统 CA bundle 和 mbedTLS 后端。
TLS 集成在 `net` 中，不新增全局 `tls` 对象或独立 Runtime 模块。

## 接口概览

以下类型用于说明接口，不表示运行时内置 TypeScript。

```ts
type IOOptions = {timeoutMs?: number};
type ReadOptions = IOOptions & {signal?: AbortSignal};
type Address = {address: string; port: number; family: 4 | 6};

type DialTCPOptions = {
  host: string;
  port: number;
  timeoutMs?: number;
  noDelay?: boolean;
};

type DialTLSOptions = DialTCPOptions & {
  serverName?: string;
  ca?: string[]; // 附加的 PEM CA 证书列表，系统 CA 始终启用
};

interface TCPConn {
  readonly localAddress: Address;
  readonly remoteAddress: Address;
  readonly closed: boolean;

  read(maxBytes: number, options?: ReadOptions): Promise<Uint8Array | null>;
  write(data: ArrayBuffer | Uint8Array | Int8Array | Uint8ClampedArray,
        options?: IOOptions): Promise<number>;
  closeWrite(options?: IOOptions): Promise<void>;
  close(): Promise<void>;
}

interface TCPListener {
  readonly localAddress: Address;
  readonly closed: boolean;
  accept(options?: IOOptions): Promise<TCPConn>;
  close(): Promise<void>;
}

net.id: string; // 只读；Wi-Fi STA MAC 的 12 位大写十六进制字符串，无分隔符
net.dialTCP(options: DialTCPOptions): Promise<TCPConn>;
net.dialTLS(options: DialTLSOptions): Promise<TCPConn>;
net.listenTCP(options: {
  host?: string; port: number; backlog?: number; timeoutMs?: number;
}): Promise<TCPListener>;
```

TLS 返回同样的字节流接口。`read` / `write` 的数据是应用明文，原生层完成
加解密；应用无法通过此句柄绕过 TLS 访问底层 TCP 字节。地址属性表示实际
TCP 地址，不是证书中的名称。

## TCP

```js
const conn = await net.dialTCP({host: "192.168.1.100", port: 9000});
try {
  await conn.write(new Uint8Array([1, 2, 3]));
  const reply = await conn.read(4096, {timeoutMs: 3000});
  if (reply !== null) console.log(reply.length);
} finally {
  await conn.close();
}
```

`dialTCP` 的 `host` 支持域名和 IPv4/IPv6 字面量，`port` 为 `1–65535`。
`noDelay` 默认 `true`，建连期限默认 `5000` ms，包含排队和 DNS。

`listenTCP` 的 `host` 默认 `"0.0.0.0"`，必须是 IP 字面量；`port` 为
`0–65535`，`0` 自动分配端口；`backlog` 默认 `4`，范围 `1–128`。
通过 `listener.localAddress.port` 获取实际端口。关闭 listener 取消待决
accept，不关闭已经接受的连接。

TCP 的完整参数、错误和资源限制见 [组件文档](../../components/js_net/README.md)。

## `net.dialTLS(options)`

```js
const conn = await net.dialTLS({
  host: "gateway.example.com",
  port: 9443,
  timeoutMs: 10000,
});
try {
  await conn.write(new Uint8Array([1, 2, 3]));
  const reply = await conn.read(4096, {timeoutMs: 3000});
} finally {
  await conn.close();
}
```

Promise 仅在 DNS、TCP 建连、TLS 握手及服务器证书验证全部成功后 resolve。
失败或超时会释放 TLS 上下文和 socket；不会返回尚在握手的连接，也不会
降级成明文 TCP。

| 字段 | 取值 | 默认值 |
| --- | --- | --- |
| `host` | 非空域名或 IPv4/IPv6 字面量，最多 255 UTF-8 字节 | 必填 |
| `port` | 整数 `1–65535` | 必填 |
| `timeoutMs` | 整数 `0–2147483647`，单位 ms | `10000` |
| `noDelay` | boolean，控制底层 TCP 的 Nagle 算法 | `true` |
| `serverName` | 要验证的 DNS 名称，同时用于 SNI | 域名 host；IP host 时必填 |
| `ca` | PEM CA 字符串数组，最多 8 项，UTF-8 内容合计最多 16 KiB | `[]`，仅使用系统 CA |

不接受 URL、`host:port` 或带 `[]` 的 IPv6 地址；IPv6 直接写地址本身。
`serverName` 必须是具体 DNS 名称，不含端口、通配符或 NUL。
未知字段、字段类型或范围错误同步抛出 `TypeError` / `RangeError`。

### 证书验证与 SNI

服务器验证始终开启，包含可信证书链、有效期及服务器名称校验。
校验按照 TLS 后端的 SAN/CN 规则执行，不只比较证书的 legacy CN 字段。
固件必须有正确的系统时间用于有效期验证；时间不正确时可能导致验证失败。
STA 获取 IP 后，固件通过 SNTP 自动校准系统时间，重连后重新发起同步。
时间服务器可通过 NVS 命名空间 `jsp32_config` 的字符串键 `ntp_server` 修改，
重启后生效。填写域名或 IP，不含协议、端口或路径，例如：

```sh
jsp32 nvs set jsp32_config ntp_server string ntp.aliyun.com
jsp32 reboot
```

未设置或值无效时，使用 `CONFIG_NETWORK_SNTP_SERVER` 默认值（`pool.ntp.org`）。
首次同步完成前，TLS 连接仍可能因时间不正确而失败，应稍后重试。
`dialTLS()` 不设置系统时钟，也不绕过有效期验证。
启用 `CONFIG_JS_NET_TLS_ENABLED` 会同时启用 mbedTLS 的证书有效期检查
（`CONFIG_MBEDTLS_HAVE_TIME_DATE`），不会依赖此前固件的默认校验开关。

`host` 决定网络连接目标，`serverName` 决定验证名称和发送的 SNI，两者
可以不同。DNS 解析得到 IP 后仍保留原始验证名称。例如固定连接地址：

```js
const conn = await net.dialTLS({
  host: "192.168.1.100",
  port: 9443,
  serverName: "gateway.example.com",
});
```

第一版连接 IP 字面量时必须显式指定 `serverName`；仅通过 IP SAN 验证的
证书不在第一版范围内。不提供 `rejectUnauthorized: false`、
`skipVerify` 或关闭 SNI/名称校验的选项。

系统 CA bundle 始终启用。本次连接的信任集合为：

```text
系统 CA bundle ∪ options.ca
```

省略 `ca` 或传入 `[]` 时只使用系统 CA。传入自定义列表后，系统 CA 仍然
有效；自定义 CA 仅附加到本次连接，不修改其他连接或全局 CA store。
固件未提供系统 bundle 时，即使指定了自定义 CA，调用也 reject `UNSUPPORTED`。

`ca` 必须是数组，每项是一个非空 PEM CA 证书字符串；不接受单个字符串、
`null`、稀疏数组、文件路径或 DER 数据。最多 8 项，全部字符串的 UTF-8
字节总数不超过 16 KiB。类型错误同步抛出 `TypeError`，数量或长度超限
同步抛出 `RangeError`。重复的有效证书不改变信任语义。

调用时复制列表及证书内容，之后修改原数组不会改变正在建立或已建立的
连接。所有证书必须解析成功才能握手；任一项格式错误 reject `TLS_CONFIG`，
不会忽略错误项后继续连接。附加信任根不关闭名称或有效期验证。

```js
// companyRootCA / companyBackupCA 是应用加载的真实 PEM CA 证书字符串。
const conn = await net.dialTLS({
  host: "192.168.1.100",
  port: 9443,
  serverName: "gateway.example.com",
  ca: [companyRootCA, companyBackupCA],
});
```

多条连接可以复用同一份应用配置；每次 dial 使用调用时的独立快照：

```js
const trustedCA = [companyRootCA, companyBackupCA];
const conn = await net.dialTLS({
  host: "gateway.example.com", port: 9443, ca: trustedCA,
});
```

第一版不提供全局 `setCA()`，避免一个脚本改变其他连接的信任策略。

### 超时、读写和关闭

`dialTLS.timeoutMs` 从调用开始计算，覆盖排队、DNS、TCP 和完整 TLS
握手；`0` 为无限等待。每个阶段不重置总期限。握手必须异步推进，不能
阻塞 JS Timer、其他连接或 Runtime 关闭。

读写仍是字节流，TLS record 边界不暴露给应用。一次 read 返回
`1–maxBytes` 字节；一次 write 可能拆成多个 TLS record，但只有整次数据
提交成功才 resolve。write 复制数据、按调用顺序发送；一个 read 和 write
可以同时等待，由同一个网络 worker 串行访问 TLS 上下文。

read 超时不破坏连接，可以继续 read。write 或 closeWrite 超时会关闭连接
并取消后续操作。TLS write 错误中的 `bytesWritten` 按已被 TLS 层接受的
成功提交的应用明文字节计数，不是 TCP 密文字节数，也不代表对端已经收到。

`closeWrite()` 必须先完成此前的 write，再发送 TLS `close_notify`；
通知发送成功后 resolve，后续 write reject `CLOSED`。不能直接用 TCP FIN
代替 TLS 关闭通知。本端 read 保持可用，直到对端按协商的 TLS 版本结束
发送；对端是否在此期间继续发送取决于 TLS 版本和对端实现。

read 收到经过 TLS 验证的 `close_notify`，先返回剩余明文，再返回 `null`。
底层 TCP 直接断开且没有合法 TLS 关闭通知时，read reject `TLS_TRUNCATED`，
随后关闭连接，以便上层区分正常 EOF 和传输截断。

`close()` 保持 TCP API 的取消语义：调用后拒绝新 I/O，取消未完成操作，
释放 TLS 和 socket 后 resolve；不保证发送 `close_notify`。正常结束发送
可先 `await conn.closeWrite()`，最终仍需 `await conn.close()`。close 幂等，
取消和 Runtime 清理不等待对端的关闭通知。

### 错误

继续使用全局 `SocketError`。建连错误的 `operation` 为 `"dialTLS"`；
后续 I/O 为 `"read"`、`"write"` 或 `"closeWrite"`。

| TLS code | 含义 |
| --- | --- |
| `UNSUPPORTED` | 固件未启用 TLS 或缺少本次所需的默认 CA bundle |
| `TLS_CONFIG` | CA 证书或 TLS 配置无法解析或使用 |
| `TLS_CERTIFICATE` | 证书链不可信、过期、尚未生效等验证失败 |
| `TLS_HOSTNAME` | 证书与 serverName 不匹配 |
| `TLS_HANDSHAKE` | 其他 TLS 握手失败，例如版本或密码套件不兼容 |
| `TLS_TRUNCATED` | 对端未正常结束 TLS 就断开底层连接 |
| `TLS_IO` | 已建立连接后的其他 TLS 协议或 record 错误 |

DNS、TCP、期限和资源错误继续使用 `DNS`、`CONNECTION_REFUSED`、
`CONNECTION_RESET`、`TIMEOUT`、`QUEUE_FULL`、`RESOURCE_LIMIT`、`CLOSED`。
原生 TLS 错误可附带 `tlsCode`，证书错误可附带 `verifyFlags`；应用应按
稳定的 `code` 分支处理，而不依赖不同 TLS 后端的原始编号。

## 与 smux 配合

TCP 和 TLS 连接都可以通过相同适配器交给 smux。当前 smux 使用空数组
表示 EOF，而 net 使用 `null`；适配器还需限制每次原生传输的大小。

```js
import {client} from "smux";

// 明文 TCP 可改用 dialTCP，传输适配器保持相同。
const conn = await net.dialTLS({host: "gateway.example.com", port: 9443});
const session = client({
  async read(maxBytes) {
    return (await conn.read(Math.min(maxBytes, 4096))) ?? new Uint8Array(0);
  },
  write(bytes) {
    return conn.write(bytes.subarray(0, 4096));
  },
  close() {
    return conn.close();
  },
}, {maxFrameSize: 4096, maxReceiveBuffer: 65536});

try {
  const stream = await session.openStream();
  try {
    await stream.write(new Uint8Array([1, 2, 3]));
    const reply = await stream.read(4096);
  } finally {
    await stream.close();
  }
} finally {
  await session.close();
}
```

适配器返回实际写入的字节数，smux 会继续发送余下内容。会话创建后由
smux 独占连接的读写和关闭。TLS 仅保护整个底层连接，不改变 smux 帧格式。

## 原生实现约定与第一版范围

TLS 使用 ESP-IDF mbedTLS 和 `esp_crt_bundle`。复用 `js_net` 的句柄、请求队列、
完成队列、DNS worker 和网络 worker；新增 TLS 握手状态和原生传输适配，
不用每连接创建任务。DNS 必须先异步完成，不能因为 TLS 建连函数内部的
解析而阻塞共享网络 worker。

原生状态按 `DNS → TCP_CONNECT → TLS_HANDSHAKE → OPEN → CLOSING → CLOSED`
推进。TLS 的 WANT_READ/WANT_WRITE 只是待就绪状态，不向 JS 报错；读操作
可能需要写就绪，写操作也可能需要读就绪。TLS 内部已有明文时必须先消费，
不能只等待 fd 可读。重试 TLS write 时必须保留该次待发送的缓冲与范围。

TLS 与 TCP 共享连接数、待决操作数及应用缓冲预算；调用时复制的 CA 字符串
也计入原生缓冲预算。`CONFIG_JS_NET_MAX_TLS_CONNECTIONS` 默认 `2`，限制
同时存活的 TLS 上下文（包含正在握手的连接），超过时 reject `RESOURCE_LIMIT`。
TLS record 缓冲由 mbedTLS 固件配置限制，CA 列表最多 8 项、16 KiB。
TLS 上下文及解析后的证书分配在原生堆中，不受 QuickJS 堆上限控制。

本项目 ESP-IDF 的 `esp-tls` 配置中，bundle 和 `cacert_buf` 走互斥分支，
不能只同时填写这两个字段来实现附加信任。本实现直接配置 mbedTLS：保留
系统 bundle 的验证回调，并为连接附加本次解析的 CA 链；不使用 `esp_crt_bundle_set()` 替换
全局 bundle，也不通过轮流修改全局 CA store 来服务不同连接。配置和解析
后的 CA 链保持有效直到对应 TLS 上下文销毁。

第一版提供经过服务器验证的 TLS 客户端。TLS 协议版本及密码套件由固件
配置决定，至少支持 TLS 1.2。不包含 `listenTLS`、双向证书认证、ALPN、
STARTTLS、会话恢复或自定义密码套件的 JS 配置。

主机验证：`python3 tests/js_net_host/run.py`。使用真实 QuickJS、ESP-IDF
mbedTLS、bundle 验证代码及本机生成的 TLS 证书，覆盖系统和附加 CA、名称与
有效期错误、连接隔离、分段读写、背压、关闭、截断及握手期间其他连接的推进。
可设置 `SANITIZE=address,undefined` 检查内存生命周期；依赖本地 ESP-IDF
源码、CMake、C 编译器以及 Python `cryptography`。不连接外部服务。

`conn.read()` 支持 `options.signal`，取消时以 `signal.reason` reject，
默认错误为 AbortError，code 为 ABORTED。连接保持打开；worker 确认前
新读取可能暂时返回 BUSY。详见[异步读取取消](abort.md)。
