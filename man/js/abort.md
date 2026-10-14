# 异步读取取消

[返回索引](index.md)

全局 `AbortController` 用于取消一次或多次异步读取。通过 options 的
`signal` 字段传入 `controller.signal`，调用 `controller.abort()` 后，
未完成的读取 Promise 立即 reject。省略 signal 时保持正常读取行为。

```js
const controller = new AbortController();
const pending = conn.read(4096, {timeoutMs: 0, signal: controller.signal});

// 不再需要这次读取时：
controller.abort();
try {
  await pending;
} catch (error) {
  if (error.code === "ABORTED") console.log("read cancelled");
  else throw error;
}
```

## 接口

| 接口 | 行为 |
| --- | --- |
| `new AbortController()` | 创建控制器及其唯一的 signal |
| `controller.signal` | 只读 `AbortSignal` |
| `controller.abort(reason?)` | 同步标记取消；重复调用无影响，保留第一次 reason |
| `signal.aborted` | 只读 boolean |
| `signal.reason` | 取消前为 undefined；取消后为传入的 reason 或默认 AbortError |
| `signal.throwIfAborted()` | 已取消时同步抛出 reason |
| `signal.addEventListener("abort", callback)` | 注册一次取消通知；相同函数不重复注册 |
| `signal.removeEventListener("abort", callback)` | 移除通知监听器 |

省略 reason 或传入 undefined 时，默认 reason 是 Error，
`name: "AbortError"`、`code: "ABORTED"`。显式 reason 可以是任意值；
读取会以同一个 reason reject。AbortSignal 不能直接构造。

同一个 signal 可以用于多个读取，abort 会取消它们全部。signal 是一次性的：
不能恢复成未取消状态，再次读取请创建新的控制器。调用读取时 signal 已取消，
Promise 直接 reject，不提交原生 I/O。signal 必须是本 Runtime 创建的
AbortSignal；伪造的 `{aborted: false}` 等对象会抛 TypeError。

支持的读取：

```js
port.read(maxBytes, {timeoutMs, signal});
port.readFrame({timeoutMs, signal});
conn.read(maxBytes, {timeoutMs, signal});          // TCP / TLS
device.read(length, {timeoutMs, signal});
device.writeRead(data, readLength, {timeoutMs, signal});
stream.read(maxBytes, {timeoutMs, signal});        // SMUX
sensor.read({timeoutMs, signal});                 // VL53L0X
sensor.readRangeMillimeters({timeoutMs, signal});
```

timeoutMs 与 signal 可以单独指定，整个 options 对象也可以省略。
等待超时的错误码仍是 `TIMEOUT`；取消、超时和正常完成，以先完成 Promise
的一方为准，迟到结果不会再次完成它。

## 原生操作与资源

取消单次读取不会关闭端口、连接、流或传感器。

- UART worker 定期检查取消标记，停止读取。已消费的字节不回放；部分帧
  丢弃并清空输入以重新同步。
- Net worker 在下一轮停止该 read；连接继续可用。worker 确认完成前，
  新 read 可能暂时返回 BUSY。
- I²C 排队中的 read/writeRead 跳过；已开始的硬件事务安全结束后释放资源，
  结果丢弃。已经发送的写阶段不能撤回。
- SMUX 解除本次等待，保留流及其接收缓存。
- VL53L0X 立即拒绝调用方的 Promise，并在当前操作安全结束后才执行
  下一个操作；已经触发的硬件测量不会撤销。

取消后的原生请求和缓冲区仍计入资源限额，直到 worker 确认完成，避免
反复取消绕过背压限制。正常完成、超时、取消、关闭都会解除相关 signal
订阅。写入、建连、accept、probe/scan 不接受 signal。
