# I²C API

[返回索引](index.md)

启用固件配置 `CONFIG_JS_I2C_ENABLED` 后，全局对象 `i2c` 提供 I²C
master controller 查询、bus 管理、设备传输、探测和扫描功能。运行期
错误使用全局 `I2CError` 类。

## 快速开始

```js
const bus = i2c.open(0, {
  sda: 8,
  scl: 9,
});

const sensor = bus.device(0x76, {clockHz: 400_000});

// START + 0x76(W) + 0xd0 + repeated START + 0x76(R) + 2 bytes + STOP
const id = await sensor.writeRead(new Uint8Array([0xd0]), 2);

await sensor.write(new Uint8Array([0xf4, 0x27]));
await bus.close();
```

I²C 总线需要 SDA 和 SCL 上拉电阻。固件可启用芯片内部上拉便于调试，
但内部上拉通常较弱，不应代替符合硬件设计的外部上拉电阻。

## 接口概览

以下声明用于描述接口，不表示固件内置 TypeScript：

```ts
type I2CData = ArrayBuffer | Uint8Array;

type I2CControllerInfo = {
  id: number;
  available: boolean;
  open: boolean;
  reason?: "not-allowed" | "reserved";
};

type I2CBusOptions = {
  sda: number;
  scl: number;
  pullups?: "external" | "internal";
  glitchIgnoreCycles?: number;
};

type I2CDeviceOptions = {
  addressBits?: 7 | 10;
  clockHz?: number;
  clockStretchTimeoutUs?: number;
};

type I2COperationOptions = {
  timeoutMs?: number;
};

type I2CScanOptions = {
  from?: number;
  to?: number;
  timeoutMs?: number;
};

type I2CErrorCode =
  | "NACK"
  | "TIMEOUT"
  | "RESOURCE_BUSY"
  | "CLOSED"
  | "QUEUE_FULL"
  | "IO";

class I2CError extends Error {
  readonly code: I2CErrorCode;
  readonly operation:
    | "open" | "device" | "probe" | "scan"
    | "read" | "write" | "writeRead" | "close";
  readonly busId: number;
  readonly address?: number;
}

i2c.list(): I2CControllerInfo[];
i2c.open(id: number, options: I2CBusOptions): I2CBus;

interface I2CBus {
  readonly id: number;
  readonly sda: number;
  readonly scl: number;
  readonly closed: boolean;

  device(address: number, options?: I2CDeviceOptions): I2CDevice;
  probe(address: number, options?: I2COperationOptions): Promise<boolean>;
  scan(options?: I2CScanOptions): Promise<number[]>;
  close(): Promise<void>;
}

interface I2CDevice {
  readonly address: number;
  readonly addressBits: 7 | 10;
  readonly clockHz: number;
  readonly closed: boolean;

  write(data: I2CData, options?: I2COperationOptions): Promise<number>;
  read(length: number, options?: I2COperationOptions): Promise<Uint8Array>;
  writeRead(
    data: I2CData,
    readLength: number,
    options?: I2COperationOptions,
  ): Promise<Uint8Array>;
  close(): Promise<void>;
}
```

## `i2c.list()`

返回 SoC 的全部 I²C controller 及实时状态：

```js
[
  {id: 0, available: true, open: false},
  {id: 1, available: false, open: false, reason: "not-allowed"},
]
```

`available` 表示 controller 通过固件 allowed mask 且未由本 API 打开。已由
本 API 打开的 controller 返回 `available: false` 和 `reason: "reserved"`。
其他原生 I²C driver 的占用状态由 ESP-IDF 在 `open` 时做最终判定；因此
`list()` 只用于查询策略和本 API 状态，`open()` 的结果才是权威结果。

## `i2c.open(id, options)`

同步创建 bus 句柄。同一 controller 不能重复打开；SDA 和 SCL 必须是
不同的有效输出 GPIO，包含在固件 allowlist 中，且未被其他模块占用。

| 字段 | 必填 | 取值 | 默认值 |
| --- | --- | --- | --- |
| `sda` | 是 | 有效且可用的 GPIO | 无 |
| `scl` | 是 | 有效且可用的 GPIO | 无 |
| `pullups` | 否 | `"external"` / `"internal"` | `"external"` |
| `glitchIgnoreCycles` | 否 | `0-255` 的整数 | `7` |

`glitchIgnoreCycles` 的单位是 I²C controller 时钟周期。未知配置字段会导致
`TypeError`。打开成功后，`bus.id` / `sda` / `scl` 为只读值，
`bus.closed` 为实时只读属性。

## `bus.device(address, options?)`

同步创建从设备句柄。`address` 不包含 R/W bit；例如数据手册中的
7-bit 地址 `0x76` 直接传 `0x76`，不左移。

| 字段 | 取值 | 默认值 |
| --- | --- | --- |
| `addressBits` | `7` / `10` | `7` |
| `clockHz` | `1-1000000` 的整数 | `100000` |
| `clockStretchTimeoutUs` | `0-2147483647` 的整数 | driver 默认值 |

7-bit 地址范围为 `0x00-0x7f`，10-bit 地址范围为 `0x000-0x3ff`。
同一 bus 可以使用不同的 `clockHz`，但不允许同时存在两个
`address + addressBits` 相同的未关闭句柄。

`device.address` / `addressBits` / `clockHz` 为只读值，`closed` 为实时
只读属性。

## `device.write(data, options?)`

执行 `START + address(W) + data + STOP`，成功时 resolve 写入字节数。

```js
await device.write(new Uint8Array([0x20, 0x01]));
```

`data` 必须是长度大于 0 的 `Uint8Array` 或 `ArrayBuffer`，不接受字符串。
数据在调用时复制，返回 Promise 后修改原 buffer 不会改变线上内容。

## `device.read(length, options?)`

执行 `START + address(R) + data + NACK + STOP`，成功时 resolve 长度恰好为
`length` 的 `Uint8Array`。I²C 不是字节流，不存在部分读取成功。

```js
const bytes = await device.read(6, {timeoutMs: 250});
```

## `device.writeRead(data, readLength, options?)`

作为一个不可分割的 bus 事务执行：

```text
START + address(W) + data + repeated START + address(R) + data + NACK + STOP
```

写阶段和读阶段之间不发送 STOP，其他 device 操作不可插入。`data` 和
`readLength` 都必须非空。

```js
async function readRegister(device, register, length) {
  return device.writeRead(new Uint8Array([register]), length);
}
```

API 不内建 `readRegister` / `writeRegister`，因为寄存器地址宽度、字节序、自增
规则和 STOP 要求属于具体设备协议，不属于 I²C 标准。

## 超时、队列和执行顺序

I/O 操作的 `timeoutMs` 默认为 `1000`，取值范围为 `0-60000`。超时从
读取（read/writeRead）的期限从调用时计算，包含 bus FIFO 排队时间；
write/probe/scan 的超时从执行时计算，scan 按每个地址计算。`0` 表示
无限等待软件完成；硬件超时和 NACK 仍会结束传输。

每个 bus 使用独立原生 worker 和有界 FIFO。同一 bus 的全部 device 共享
队列，调用顺序就是线上执行顺序。worker 不调用 QuickJS，完成结果通过
EventLoop 解析 Promise。队列满时 Promise 以 `QUEUE_FULL` reject。

单个读或写阶段的长度由 `CONFIG_JS_I2C_MAX_TRANSFER_SIZE` 限制，默认最大
4096 bytes；每条 bus 的未完成操作数由 `CONFIG_JS_I2C_MAX_PENDING` 限制，
默认为 16。

## `bus.probe(address, options?)`

探测一个 7-bit 地址。地址 ACK 时 resolve `true`，NACK 时 resolve `false`。
超时或其他 driver 错误 reject。默认超时为 10 ms。

```js
if (await bus.probe(0x76)) {
  console.log("sensor found");
}
```

## `bus.scan(options?)`

按从小到大顺序探测 7-bit 地址，resolve 已 ACK 的数字数组。

```js
const addresses = await bus.scan();
console.log(addresses.map(address => `0x${address.toString(16)}`));
```

| 字段 | 取值 | 默认值 |
| --- | --- | --- |
| `from` | `0x00-0x7f` | `0x08` |
| `to` | `0x00-0x7f` | `0x77` |
| `timeoutMs` | 每个地址 `0-60000` ms | `10` |

`from` 不能大于 `to`。整次扫描是 FIFO 中的单个操作，扫描期间不执行其他
device I/O。NACK 会继续扫描；任一地址超时或出现 driver 错误时，整个
Promise reject，不返回部分结果。

## 关闭与生命周期

`device.close()` 和 `bus.close()` 都返回 Promise，并且可重复安全调用。

- `device.close()` 调用后句柄立即不再接受新操作，它在队列中更早的操作
  完成并从 driver 移除设备后 resolve。
- `bus.close()` 调用后 bus 及其全部 device 立即不再接受新操作。当前
  I/O 受自身超时限制；已排队但未开始的 I/O 以 `CLOSED` reject。驱动和
  引脚释放后 `close()` resolve。
- bus 关闭后，从它创建的全部 device 都永久失效。Runtime 停止时会强制
  关闭所有 bus 并 reject 未完成 Promise。

## I²C 错误

参数类型错误、缺少必填字段或未知字段同步抛出 `TypeError`；数字越界
同步抛出 `RangeError`。资源冲突、关闭句柄以及进入队列后的失败使用
`I2CError`。

```js
try {
  await sensor.read(2, {timeoutMs: 100});
} catch (error) {
  if (error instanceof I2CError && error.code === "NACK") {
    console.log("sensor did not acknowledge");
  } else {
    throw error;
  }
}
```

| `code` | 含义 |
| --- | --- |
| `NACK` | 地址或数据阶段未收到 ACK |
| `TIMEOUT` | bus 忙、硬件卡死或传输在 `timeoutMs` 内未完成 |
| `RESOURCE_BUSY` | controller、GPIO 或相同地址句柄已被占用 |
| `CLOSED` | bus/device 已关闭，或 bus 关闭取消了未开始的操作 |
| `QUEUE_FULL` | bus 的有界操作队列已满 |
| `IO` | 无法更精确归类的 driver、硬件或内存错误 |

`message` 用于日志，程序分支应使用 `code`。`operation` 表示失败的 API，
`busId` 表示 controller，设备相关错误还包含 `address`。ESP-IDF 将 bus 忙和
硬件传输逾期统一返回为 timeout，API 不尝试猜测无法稳定区分的原因。
