# Smux v1

[返回索引](../index.md)

手册主题：`js.stdlib.smux`。命令行执行 `cli man js.stdlib.smux`，
HTTP 访问 `GET /api/v1/man/js.stdlib.smux`。

`/stdlib/smux.js` 实现 [xtaci/smux](https://github.com/xtaci/smux) 的 v1
字节流多路复用协议。模块不依赖网络 API，底层可以是任意可靠、有序的双向
字节传输。仅支持 v1，两端必须配置相同版本；不支持 v2 的 UPD 窗口流控。

```js
import {client, server} from "smux";

// reader / writer 由应用提供，不要求特定的网络实现。
const session = client({
  read: maxBytes => reader.read(maxBytes),
  write: bytes => writer.write(bytes),
  close: () => connection.close(),
});
try {
  const stream = await session.openStream();
  try {
    await stream.write(new Uint8Array([1, 2, 3]));
    const reply = await stream.read(1024);
    console.log(stream.id, reply.length);
  } finally {
    await stream.close();
  }
} finally {
  await session.close();
}

// 对端使用 server(transport)，通过 await session.acceptStream() 接收流。
```

## 抽象传输约定

| 接口 | 约定 |
| --- | --- |
| `read(maxBytes)` | 返回或异步返回 `Uint8Array`，长度为 1..maxBytes；空数组表示 EOF，失败抛出错误 |
| `write(bytes)` | 返回或异步返回实际写入的正整数字节数；返回 `undefined` 表示全部写入；失败抛出错误 |
| `close()` | 可选，释放底层传输并结束正在等待的读写，可以返回 Promise |

传输适配器需要保留超过 `maxBytes` 的输入，供下次读取；返回过量数据会终止
会话。协议层自动补齐短读、重试短写，并串行调用底层 write，防止帧交错。
同一会话同时最多有一个底层 read 和一个底层 write；两者可同时进行。
write 完成前底层可以使用传入的数组。应用创建会话后应由它独占底层读写。

会话关闭时立即拒绝等待中的协议操作，并调用可选的 transport.close。
适配器应在 close 中中断底层读写；没有 close 时，由应用负责释放底层资源。

## 接口

`client(transport, options?)`、`server(transport, options?)` 创建会话并立即启动
接收循环，也可使用 `new Session(transport, {...options, client: true/false})`。

| 会话接口 | 行为 |
| --- | --- |
| `await session.openStream()` | 发送 SYN，返回新流，无需等待对端确认 |
| `await session.acceptStream()` | 等待对端 SYN，按到达顺序返回流 |
| `await session.close(reason?)` | 关闭整个会话，重复调用返回同一 Promise |
| `session.closed` / `session.error` | 是否关闭，以及关闭原因，未关闭时 error 为 null |
| `session.numStreams` | 当前尚未在本地关闭的流数量 |

| 流接口 | 行为 |
| --- | --- |
| `stream.id` / `stream.closed` | uint32 流 ID / 是否在本地关闭 |
| `await stream.read(maxBytes = 32768, options?)` | 返回当前可读的最多 maxBytes 个字节；对端 FIN 后先读完缓存，再返回空数组 |
| `await stream.write(bytes)` | 写入 Uint8Array，按 maxFrameSize 分帧，返回完整写入的字节数 |
| `await stream.close()` | 丢弃该流接收缓存并发送 FIN，重复调用返回同一 Promise |

`read` 的 options 为 `{timeoutMs?: number, signal?: AbortSignal}`；timeoutMs
为整数 0..60000，默认 0 表示无限等待。有限超时从调用时计算，超时 reject，错误带
`code: "TIMEOUT"` 和 `operation: "read"`；流仍可继续读取。

流是字节流，read 的边界与 write 的边界无关。每个流只允许一个待完成的
read，重复读取报 `BUSY`；并发 write 按调用顺序执行，调用时复制输入数据。
接收 FIN 结束读取，仍可向对端写入响应；应用处理完后须主动 close 释放流。
本地 close 使后续 write 失败，并唤醒待完成的 read，返回空数组。
会话异常或关闭则让待完成的 read、write、acceptStream 和 openStream 失败。
write 中途失败时，错误的 `bytesWritten` 表示已完整发送的 payload 字节数。

## 配置与协议

| options 字段 | 默认值 | 说明 |
| --- | --- | --- |
| `maxFrameSize` | 32768 | 发送 payload 的最大长度，1..65535 |
| `maxReceiveBuffer` | 4194304 | 会话共享的接收缓存阈值，正整数 |
| `maxPendingStreams` | 1024 | 等待 accept 的流数量上限，正整数 |
| `keepAliveDisabled` | false | 禁用周期 NOP 和空闲超时检测 |
| `keepAliveIntervalMs` | 10000 | NOP 周期，正整数 |
| `keepAliveTimeoutMs` | 30000 | 接收空闲超时，必须 >= 周期 |

内存较小的设备可降低接收缓存，例如 `{maxReceiveBuffer: 65536}`。缓存达到
阈值后暂停底层读取，应用 read 或 close 释放缓存后恢复。与 Go smux 一样，
正在接收的完整帧允许超过阈值，最多额外缓存 65534 字节。
maxFrameSize 只限制发送，不限制对端发送的合法帧。接收缓存满时不触发空闲
超时；应用须持续消费数据，否则所有流的接收会停顿。等待 accept 的队列满
时也暂停读取。未关闭的活跃流与应用提交的待写数据需要由应用控制数量。

帧头固定为 8 字节：`VERSION(1) | CMD(1) | LENGTH(2) | STREAMID(4)`。
LENGTH、STREAMID 使用小端序。版本必须为 1；命令为 SYN=0、FIN=1、PSH=2、
NOP=3，只有 PSH 可携带数据。客户端 ID 使用奇数，服务端使用偶数，匹配 Go
实现的首次分配 3 / 2；耗尽后 openStream 报 `GO_AWAY`，应创建新会话。
重复 SYN 忽略；未知流的 PSH/FIN 丢弃。非法版本、命令或控制帧长度会终止
会话并报 `PROTOCOL`。底层 EOF 报 `EOF`，保活超时报 `TIMEOUT`，关闭报
`CLOSED`。底层 I/O 错误保留原始原因。

验证命令：`node tests/smux.test.mjs`。实际 Go 互通验证：
`node tests/smux_host/run.mjs`，需要 Go 和 github.com/xtaci/smux v1.5.35，
通过标准输入/输出测试 JS 客户端和服务端各三个并发流，无需网络连接。

读取支持可选的 `signal`。取消时以 `signal.reason` reject，默认错误为
`AbortError`（`code: "ABORTED"`），不关闭句柄；详见[异步读取取消](../abort.md)。
