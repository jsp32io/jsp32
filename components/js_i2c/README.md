# JavaScript I²C API

`js_i2c` 通过 `js_runtime` 外部模块接口安装全局 `i2c` 对象和
`I2CError` 类。API 仅支持 I²C master。

```js
const bus = i2c.open(0, {sda: 8, scl: 9});
const sensor = bus.device(0x76, {clockHz: 400_000});

const id = await sensor.writeRead(new Uint8Array([0xd0]), 2);
await sensor.write(new Uint8Array([0xf4, 0x27]));

await bus.close();
```

## Bus

`i2c.list()` 返回 controller 状态：

```js
[{id: 0, available: true, open: false}]
```

`i2c.open(id, options)` 的配置字段：

| 字段 | 取值 | 默认值 |
| --- | --- | --- |
| `sda` | 可用 GPIO | 必填 |
| `scl` | 可用 GPIO | 必填 |
| `pullups` | `"external"` / `"internal"` | `"external"` |
| `glitchIgnoreCycles` | `0-255` | `7` |

内部上拉只适合调试或低速短距离总线，不能代替合适的外部上拉电阻。
引脚必须在固件 allowlist 内，且未被 GPIO、UART 或其他原生驱动占用。

Bus 提供：

- `device(address, options?)`：创建设备句柄。
- `probe(address, {timeoutMs?})`：探测 7-bit 地址，ACK 返回 `true`，NACK
  返回 `false`。
- `scan({from?, to?, timeoutMs?})`：扫描 7-bit 地址，默认范围
  `0x08-0x77`。
- `close()`：取消未开始的操作，释放 driver 和引脚后 resolve。

`bus.id` / `sda` / `scl` 是只读值，`bus.closed` 是实时只读属性。

## Device

`bus.device(address, options?)` 使用不包含 R/W bit 的原始 7-bit 或 10-bit
地址：

| 字段 | 取值 | 默认值 |
| --- | --- | --- |
| `addressBits` | `7` / `10` | `7` |
| `clockHz` | `1-1000000` | `100000` |
| `clockStretchTimeoutUs` | 非负整数 | driver 默认值 |

同一 bus 可以有不同时钟的设备，但不能重复打开同一
`address + addressBits`。`address` / `addressBits` / `clockHz` 是只读值，
`closed` 是实时只读属性。

Device 提供：

```js
await device.write(data, {timeoutMs: 1000});
const bytes = await device.read(length, {timeoutMs: 1000});
const bytes = await device.writeRead(data, readLength, {timeoutMs: 1000});
await device.close();
```

`data` 必须是 `Uint8Array` 或 `ArrayBuffer`，不接受字符串。写数据在
调用时复制。`writeRead` 在写和读之间使用 repeated START，整个事务
不会被其他设备操作插入。

同一 bus 的全部操作共享有界 FIFO，线上顺序与 JavaScript 调用顺序一致。
I/O 在 bus worker 中执行，QuickJS EventLoop 不会被阻塞。

`timeoutMs` 为整数 0..60000，read/writeRead 默认 1000 ms，0 表示无限
等待软件完成。读取期限包含排队时间；等待超时 reject `I2CError`，
`code` 为 `TIMEOUT`。硬件超时和 NACK 仍会结束传输。
write/probe/scan 使用同样的零值含义，其超时从执行时计算，scan 按每个地址计算。

## Errors and lifetime

参数类型和范围错误同步抛出 `TypeError` / `RangeError`。资源冲突和
异步 I/O 失败使用 `I2CError`：

```js
try {
  await device.read(2);
} catch (error) {
  if (error instanceof I2CError && error.code === "NACK") {
    // device did not acknowledge
  }
}
```

`code` 可为 `NACK`、`TIMEOUT`、`RESOURCE_BUSY`、`CLOSED`、
`QUEUE_FULL` 或 `IO`。错误还包含 `operation`、`busId` 和可选的
`address`。

`device.close()` 和 `bus.close()` 都是幂等的。调用后句柄立即不再接受
新操作，Promise 在原生资源完成释放后 resolve。Runtime 停止时会关闭
所有 bus 并 reject 未完成操作。
