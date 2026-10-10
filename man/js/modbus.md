# Modbus 标准库

[返回索引](index.md)

`/stdlib/modbus.js` 提供 Modbus RTU 从站协议，使用 [UART](uart.md) 分帧
读写及 [Registers](registers.md) 内存寄存器。通过 `"modbus"` 导入。

```js
import {createSlave} from "modbus";

const port = uart.open(1, {
  tx: 17, rx: 18, baudRate: 9600, dataBits: 8, parity: "even",
  framing: {type: "idle", gapChars: 3.5, maxBytes: 256},
  halfDuplex: {directionPin: 16}, // TTL 或 RS-232 可省略
});
const slave = createSlave({port, unitId: 1});
const serving = slave.serve();
serving.catch(error => console.log("Modbus stopped:", error));
// 停止时：slave.stop(); await serving; port.close();
```

## `createSlave(options)`

返回 `{handleRequest, serve, stop}` 对象。

| 选项 | 含义 | 默认值 |
| --- | --- | --- |
| `port` | 提供 readFrame 和 write 的端口 | 必填 |
| `unitId` | 从站地址，整数 1..247 | 1 |
| `map` | 显式区域映射 | 根据寄存器容量生成 |
| `registers` | 寄存器接口对象，需提供 status、readBits、writeBits、readWords、writeWords | `globalThis.registers` |
| `readTimeoutMs` | 正整数，单次 readFrame 超时 | 100 |
| `maxTransaction` | 正整数，每次寄存器操作数量上限 | 256 |

maxTransaction 不应超过固件 `APP_REGISTERS_MAX_TRANSACTION` 配置。
无效接口对象抛出 TypeError，无效 unitId、超时或事务上限抛出 RangeError。

## 地址映射

默认映射随 `registers.status().capacity` 变化。固件默认容量下：

| Modbus 区域 | PDU 地址（从 0 开始） | 应用寄存器 | Modbus 权限 |
| --- | --- | --- | --- |
| Discrete inputs | 0..255 | X0..X255 | 只读 |
| Coils | 0..255 | Y0..Y255 | 读写 |
| Coils | 256..1279 | M0..M1023 | 读写 |
| Input registers | 0..1023 | D0..D1023 | 只读 |
| Holding registers | 0..1023 | D1024..D2047 | 读写 |

Coils 先映射全部 Y，再接全部 M；D 前半段映射 input，后半段映射
holding。D 容量为奇数时，可写的后半段多一个。这只限制 Modbus 访问，
JS 仍能写全部 D 寄存器。

显式 map 会替换默认映射：

```js
const slave = createSlave({port, unitId: 1, map: {
  coils: [{start: 0, address: "M100", count: 32}],
  holdingRegisters: [{start: 0, address: "D100", count: 32}],
}});
```

区域键为 coils、discreteInputs、holdingRegisters、inputRegisters。
start 是从 0 开始的 PDU 地址；客户端显示的 holding register 40001 通常对应
start 0。count 必须在应用寄存器容量内，同一区域的映射段不能重叠。
每个请求必须完整落在同一映射段中，跨段或未映射请求返回异常 02。
X 只能映射为 discreteInputs；Y/M 可映射为 coils 或 discreteInputs；
D 可映射为 holdingRegisters 或 inputRegisters。

## 从站方法

| 方法 | 返回值 | 行为 |
| --- | --- | --- |
| `slave.handleRequest(frame)` | `Uint8Array` 或 null | 同步处理含 CRC 的 RTU 请求，返回含 CRC 的响应；不操作串口 |
| `slave.serve()` | `Promise<void>` | 循环读一帧、处理并发送响应，直到停止或 I/O 失败 |
| `slave.stop()` | `undefined` | 请求停止，不关闭 port |

支持功能码 01、02、03、04、05、06、0F、10。校验帧长和 CRC，非本机地址
或坏 CRC 静默丢弃；unit ID 0 作为只写广播处理，不返回响应。
非法功能、地址、值以及内部处理失败返回对应 Modbus 异常。
handleRequest 对无效帧或无需响应的请求返回 null。

serve 不能并发运行。stop 等当前 I/O 完成后生效，停止完成后可再次 serve。
UART I/O 失败会使 serve 拒绝，调用方应处理异常并决定是否重启。
端口由调用方创建和关闭。

原生 Modbus TCP Server 使用同一套默认寄存器映射，其配置方式见
[Modbus TCP Server](../../components/modbus_tcp/README.md)。

## `crc16(bytes)`

```js
import {crc16} from "modbus";
const crc = crc16(new Uint8Array([1, 3, 0, 0, 0, 1]));
console.log(crc);
```

对字节序列计算 Modbus CRC16，返回 0..65535 的整数；输入不包含待添加的
CRC。组帧时先写 CRC 低字节，再写高字节。通常使用 Uint8Array 作为输入。

## 时序限制

UART 当前把 3.5 字符的空闲间隔向上取整为 4，并非精确的 RTU t3.5/t1.5
实现。合法但只间隔 3.5 至不足 4 字符的背靠背帧可能合并，协议库不能恢复
底层未记录的时序。互通时需要留更大的帧间隔。
