# UART API

[返回索引](index.md)

启用 `CONFIG_JS_UART_ENABLED` 后提供全局 `uart`。打开端口同步完成，
读写返回 Promise，通过原生 worker 执行。

```js
const port = uart.open(1, {tx: 17, rx: 18, baudRate: 115200});
try {
  console.log(await port.write("AT\r\n"));
  const data = await port.read(256, {timeoutMs: 1000});
  console.log(data); // Uint8Array，等待超时则 reject TIMEOUT
} finally {
  port.close();
}
```

## 控制器与配置

`uart.list()` 返回 `{id, available, open, reason?}` 数组。不可用时 reason
为 `"not-allowed"`、`"console"` 或 `"reserved"`。

`uart.open(id, options)` 返回端口句柄，options 支持：

| 字段 | 取值 | 默认值 |
| --- | --- | --- |
| `tx` | 可用 GPIO | 省略 |
| `rx` | 可用 GPIO | 省略 |
| `baudRate` | 整数 300..5000000 | 115200 |
| `dataBits` | 5、6、7、8 | 8 |
| `stopBits` | 1、1.5、2 | 1 |
| `parity` | `"none"`、`"even"`、`"odd"` | `"none"` |
| `rxBufferSize` | 整数 256..16384 | 固件配置 |
| `txBufferSize` | 整数 256..16384 | 固件配置 |
| `framing` | 下述分帧对象 | 字节流模式 |
| `halfDuplex` | `{directionPin: GPIO}` | 省略 |

至少指定 tx、rx 之一；引脚必须有效、位于固件允许范围内且未被占用。
未知字段报错，同一端口不能重复打开。

halfDuplex 将 directionPin 用作 UART RTS，连接收发器高有效 DE 和低有效
/RE（例如将 MAX3485 的 DE 与 /RE 相连）。驱动在发送时拉高 RTS，最后一个
bit 发送结束后拉低。TTL、RS-232、RS-485 共用同一套字节 API。

## 字节读写

| 方法 | 返回值 | 说明 |
| --- | --- | --- |
| `port.write(data, options?)` | `Promise<number>` | 硬件发送完成后返回字节数 |
| `port.read(maxBytes, options?)` | `Promise<Uint8Array>` | 有数据时返回最多 maxBytes 字节 |
| `port.readFrame(options?)` | `Promise<Uint8Array>` | 返回一帧，仅用于分帧模式 |
| `port.close()` | `undefined` | 关闭端口并释放资源 |

write 接受 UTF-8 字符串、Uint8Array 或 ArrayBuffer。
写入 options 可省略、传 undefined 或空对象 `{}`；暂不支持任何字段，
包括 timeoutMs 和 signal，传入时抛 TypeError。写入仍在硬件发送完成后 resolve。
read 不保证读满 maxBytes；等到至少一个字节即可返回。
options 为 `{timeoutMs?: number, signal?: AbortSignal}`。
timeoutMs 为整数 0..60000，默认 0 表示无限等待。
有限超时从调用时计算，包含排队时间；超时 reject Error，带有
`code: "TIMEOUT"` 和 `operation: "read"` 或 `"readFrame"`。
超时后端口仍可使用。旧的数字超时参数不接受。

同一端口所有读写按调用顺序执行，前面的长时间 read 会延迟后续 write。
双向通信应使用有限读取超时。参数校验错误同步抛出；驱动错误和操作队列满
通过 Promise 拒绝报告。传输大小和待处理操作数量受固件配置限制。

## 分帧读取

在 open 时设置 framing 后，通过 readFrame 取得完整帧，不可混用 read。

```js
const port = uart.open(1, {
  tx: 17, rx: 18, baudRate: 9600, parity: "even",
  framing: {type: "idle", gapChars: 3.5, maxBytes: 256},
  halfDuplex: {directionPin: 16},
});
const frame = await port.readFrame({timeoutMs: 1000});
```

| type | 配置 | 完帧条件 |
| --- | --- | --- |
| `"idle"` | gapChars 默认 4，范围 1..126 | 持续空闲指定字符时间 |
| `"fixed"` | length 必填 | 收到固定字节数 |
| `"delimiter"` | byte 必填，0..255 | 收到分隔符，返回帧包含该字节 |
| `"lengthField"` | offset 默认 0；width 为 1 或 2，默认 1；endian 为 `"big"` 或 `"little"`，默认 little；adjustment 默认 0 | 总长度 = 长度字段值 + adjustment |

所有模式支持 maxBytes，默认 256，不能超过固件最大传输长度。
超长帧使 Promise 拒绝，并清空 RX 缓冲以重新同步。
readFrame 的 timeoutMs 是整个等待期限，不是分帧规则；
无输入或收到部分帧但未完成时都 reject TIMEOUT。部分帧会清空以重新同步。

idle 模式按完整 UART 字符周期计时，gapChars 向上取整，例如 3.5 实际为 4。
该规则检测帧结束，不单独校验协议规定的最大帧内字节间隔。

## 关闭与重用

close 可重复调用。它停止 worker，以 `UART closed` 拒绝未完成操作，
释放驱动和引脚，使旧句柄失效；随后可以重新 open 同一控制器。
Runtime 退出会关闭全部端口。

协议示例见 [Modbus 标准库](modbus.md)。

读取支持可选的 `signal`。取消时以 `signal.reason` reject，默认错误为
`AbortError`（`code: "ABORTED"`），不关闭句柄；详见[异步读取取消](abort.md)。
