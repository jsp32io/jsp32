# 寄存器使用手册

[返回 JavaScript API 索引](index.md)

JSP32 的 `registers` 是设备内存中的一张寄存器表，可让脚本、上位机、HTTP
客户端和 Modbus 从站交换状态与数据。启用 `CONFIG_JS_REGISTERS_ENABLED` 后，
JavaScript 可直接使用全局对象 `registers`，无需 `import`。例如：

```js
registers.write("D100", 42);       // 写入一个 16 位数据字
console.log(registers.read("D100")); // 42
```

**寄存器值只保存在 RAM 中，设备重启后全部归零。** `X/Y` 的名称表示输入和输出
用途，并不意味着它们自动连接到 GPIO；实际数据由使用这些寄存器的原生驱动、
脚本或协议来读写。JavaScript 可以写入 `X`，但 HTTP、CLI 和 Modbus 仍将其作为
只读输入位。

DI 输入、边沿计数、运行时间和编码器计长的寄存器约定见 [io 插件使用手册](plugins/io.md)。

## 地址和数值

地址由一个大写区域字母和从 `0` 开始的十进制索引组成。例如 `X0`、`M42`、
`D100`；`D0100`、`d100` 和超出区域容量的地址都无效。

| 区域 | 用途 | JavaScript 读到的值 | JavaScript 能否写入 |
| --- | --- | --- | --- |
| `X` | 输入位 | `false` / `true` | 是 |
| `Y` | 输出位 | `false` / `true` | 是 |
| `M` | 内部状态位 | `false` / `true` | 是 |
| `D` | 数据字，每个占 16 位 | `0`～`65535` 的整数 | 是 |

写入 `X/Y/M` 时可以用 `false`、`true` 或数字 `0`、`1`；写入 `D` 时只能用
`0`～`65535` 的整数。`D` 本身没有 `int32` 或 `float32` 类型，较宽的数据
需要按约定放进相邻的多个 D 字。

当前设备的容量可通过 `registers.status()` 查看：

```js
const {capacity, revision, watcherCount} = registers.status();
console.log(capacity.D); // D 区可用字数
```

默认容量为 X/Y 各 256 位、M 为 1024 位、D 为 2048 个字；固件配置可以修改这些值，编写
脚本时应以设备实际返回的 `capacity` 为准。`revision` 是寄存器表的全局修订
号，`watcherCount` 是当前 JavaScript 监听器数量。

## 读写一个地址

```js
registers.write("M10", true); // 标记设备处于运行状态
registers.write("D100", 25);  // 当前产量

const running = registers.read("M10"); // true
const output = registers.read("D100"); // 25
```

写入成功返回当前 `revision`。如果写入值和原值相同，修订号不变，也不会产生
变更事件。尝试写非法值或越界地址会抛出异常。

## 连续读写

相邻的位使用 `readBits` / `writeBits`，相邻的 D 字使用 `readWords` /
`writeWords`：

```js
registers.writeBits("M100", [true, false, true]); // M100～M102
const bits = registers.readBits("M100", 3);       // Uint8Array [1, 0, 1]

registers.writeWords("D100", [10, 20, 30]);      // D100～D102
const words = registers.readWords("D100", 3);     // Uint16Array [10, 20, 30]
```

`readBits` 只接受 `X/Y/M`，`readWords` 只接受 `D`。写入长度或读取数量必须
大于零，且不得超过固件配置的单次事务上限；默认上限是 256。范围不能越过该
区域末尾。一次范围读取取得同一时刻的快照；一次范围写入先校验全部值，再
一起提交。任何一项无效时，整次写入都不会生效。

### 用两个 D 字表示 `int32`

上位机中将监控设为 **D100 / int32 / ABCD** 时，D100 放高 16 位，D101
放低 16 位。脚本可以按同样约定读写：

```js
const production = 123456;
registers.writeWords("D100", [production >>> 16, production & 0xffff]);

const [high, low] = registers.readWords("D100", 2);
const currentProduction = (high << 16) | low; // 有符号 int32：123456
```

这是数据解释约定，不是 D 区的固有类型。上位机监控可选择其他字节序；脚本
和上位机必须使用相同的类型、起始地址和字节序。一个 `int32` 监控占用两个
连续 D 字，避免再把 D101 用作独立数据。

## 一次读写多个不相邻地址

```js
const state = registers.readMany(["X0", "M10", "D100"]);
// 例如 {X0: false, M10: true, D100: 25}

const revision = registers.writeMany({
  M10: true,
  D100: 26,
  D200: 50,
});
```

`readMany` 返回以地址为键的对象，传入的地址不能重复。`writeMany` 适合同时
更新多个相关值，整次调用是一个事务；有值变化时全局 `revision` 只增加一次。
单次地址数量不能超过事务上限。

## 监听变化

当脚本需要在值变化时执行动作，可注册监听器：

```js
const stopWatching = registers.watch(["M10", "D100"], event => {
  console.log(event.revision, event.source);
  for (const change of event.changes) {
    console.log(change.address, change.oldValue, change.value);
  }
});

// 之后不再需要时调用 stopWatching()，释放监听器名额。
```

只监听一个地址时可写 `registers.watch("D100", callback)`。回调在
QuickJS EventLoop 中异步执行。一次回调对应一次已提交的写入事务，
`event.changes` 只列出本监听器关注且确实变化的地址。`source` 可为
`"js"`、`"http"`、`"driver"` 或 `"system"`。监听器数量和每个监听器的
地址数量有固件配置上限；默认分别为 32 和 64。脚本或插件不再使用监听器
时应调用返回的取消函数。

## 在上位机和 CLI 中查看

设备内置网页的 `/registers` 页面可只读浏览 X/Y/M/D 寄存器，支持分页、地址
跳转和实时更新。设备重启后所有值仍会归零。

桌面上位机选中设备后进入“寄存器”页，可添加名称、地址、数据类型和字节序
组成的监控，也可按 X/Y/M/D 区域浏览当前值。监控定义保存在电脑上，
寄存器当前值仍在设备 RAM 中。上位机操作见 [桌面控制台说明](../../gui/README.md)。

命令行也能读取设备当前寄存器，不需要通过 JavaScript eval：

```sh
cd cli
go build -o jsp32 .
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reg info
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reg get M10 D100
JSP32_BASE_URL=http://192.168.1.20 ./jsp32 reg watch D100
```

CLI 用法见 [CLI 文档](../../cli/README.md)；原生 HTTP 请求格式见
[寄存器 HTTP API](../../components/reg_http/README.md)。这些入口访问的是同一张
寄存器表，但 JavaScript 的 `revision` 是数字，HTTP 响应中的 `revision`
是十进制字符串。

## JavaScript 接口速查

以下签名只用于说明参数和返回值，并不表示固件内置 TypeScript：

```ts
registers.read(address: string): boolean | number;
registers.write(address: string, value: boolean | number): number;
registers.readBits(start: string, count: number): Uint8Array;
registers.writeBits(start: string, values: ArrayLike<boolean | number>): number;
registers.readWords(start: string, count: number): Uint16Array;
registers.writeWords(start: string, values: ArrayLike<number>): number;
registers.readMany(addresses: string[]): Record<string, boolean | number>;
registers.writeMany(values: Record<string, boolean | number>): number;
registers.watch(addresses: string | string[], callback: (event: {
  revision: number;
  source: "js" | "http" | "driver" | "system";
  changes: {address: string; oldValue: boolean | number; value: boolean | number}[];
}) => void): () => undefined;
registers.status(): {
  revision: number;
  watcherCount: number;
  capacity: {X: number; Y: number; M: number; D: number};
};
```
