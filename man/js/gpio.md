# GPIO API

[返回索引](index.md)

启用固件配置 `CONFIG_JS_GPIO_ENABLED` 后，全局对象 `gpio` 提供 GPIO 查询、
配置、读写、边沿监听和复位功能。

## 快速开始

将 GPIO 2 配置为输出并拉高：

```js
gpio.setup(2, {mode: "output", initial: 1});
```

也可以先配置，再写入高电平：

```js
gpio.setup(2, {mode: "output"});
gpio.write(2, 1);
```

通过 CLI 执行多条语句时使用 script 模式：

```sh
jsp32 eval --mode script \
  "gpio.setup(2, {mode: 'output'}); gpio.write(2, 1);"
```

如果需要同时读回 GPIO API 保存的输出电平，可以使用一个表达式：

```sh
jsp32 eval \
  "(gpio.setup(2, {mode: 'output', initial: 1}), gpio.read(2))"
```

返回值为 `1`。

> 在连接外部电路前，请确认所选 GPIO 对目标开发板可用，并确保电压、电流和
> 负载符合 ESP32-S3 及开发板的电气限制。

## 接口概览

以下声明用于描述接口，并不表示固件内置 TypeScript：

```ts
type GPIOLevel = 0 | 1 | boolean;
type GPIOEdge = "rising" | "falling" | "both";

type GPIOInputOptions = {
  mode: "input";
  pull?: "none" | "up" | "down";
  debounceMs?: number;
};

type GPIOOutputOptions = {
  mode: "output";
  initial?: GPIOLevel;
};

type GPIOPinInfo = {
  pin: number;
  available: boolean;
  configured: boolean;
  mode: "input" | "output" | null;
  reason?: "reserved" | "not-allowed";
};

type GPIOEvent = {
  pin: number;
  value: 0 | 1;
  edge: "rising" | "falling";
  timestampUs: number;
};

gpio.list(): GPIOPinInfo[];
gpio.setup(pin: number, options: GPIOInputOptions | GPIOOutputOptions): undefined;
gpio.read(pin: number): 0 | 1;
gpio.write(pin: number, value: GPIOLevel): undefined;
gpio.watch(pin: number, edge: GPIOEdge, callback: (event: GPIOEvent) => void): () => undefined;
gpio.reset(pin: number): boolean;
```

## 可用引脚

ESP32-S3 的有效 GPIO 编号为 `0-21` 和 `26-48`。这不代表每个有效编号在当前
开发板和运行状态下都可供应用使用。Flash、PSRAM、USB 或其他原生驱动占用的
引脚可能被保留；原生固件还可以通过 allowed-pin mask 禁止特定引脚。

应在配置前调用 `gpio.list()`，以设备实时返回的 `available` 为准：

```js
gpio.list();
```

返回数组中的每一项包含：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `pin` | `number` | GPIO 编号 |
| `available` | `boolean` | 当前是否允许配置 |
| `configured` | `boolean` | 是否已由 JavaScript GPIO API 配置 |
| `mode` | `"input" \| "output" \| null` | 当前模式；未配置时为 `null` |
| `reason` | `string` | 不可用时为 `reserved` 或 `not-allowed` |

示例：

```js
[
  {pin: 0, available: true, configured: false, mode: null},
  {
    pin: 19,
    available: false,
    configured: false,
    mode: null,
    reason: "reserved"
  }
]
```

`reserved` 表示引脚当前被 SoC 功能或其他驱动保留；`not-allowed` 表示原生固件
的引脚策略不允许使用。部分有效 GPIO 可能只支持输入；尝试将其配置为输出会
抛出异常。

## `gpio.setup(pin, options)`

配置一个 GPIO。成功时返回 `undefined`。

输出模式：

```js
gpio.setup(2, {
  mode: "output",
  initial: 1,
});
```

| 字段 | 必填 | 取值 | 默认值 |
| --- | --- | --- | --- |
| `mode` | 是 | `"output"` | 无 |
| `initial` | 否 | `0`、`1`、`false`、`true` | `0` |

固件会先设置输出锁存值，再启用输出模式，以避免配置过程中产生不期望的电平
毛刺。`pull` 和 `debounceMs` 不能用于输出模式。

输入模式：

```js
gpio.setup(4, {
  mode: "input",
  pull: "up",
  debounceMs: 30,
});
```

| 字段 | 必填 | 取值 | 默认值 |
| --- | --- | --- | --- |
| `mode` | 是 | `"input"` | 无 |
| `pull` | 否 | `"none"`、`"up"`、`"down"` | `"none"` |
| `debounceMs` | 否 | `0-60000` 的整数，单位毫秒 | `0` |

`initial` 不能用于输入模式。未知字段、字段类型错误以及与模式不匹配的字段都会
导致异常。

使用相同的规范化配置重复调用 `setup` 是空操作，已有 watcher 会保留。修改
配置则会先删除该引脚上的全部 watcher。省略的默认值参与配置比较，例如：

```js
gpio.setup(4, {mode: "input"});
gpio.setup(4, {mode: "input", pull: "none", debounceMs: 0});
```

以上两次配置相同。

## `gpio.read(pin)`

读取引脚的电平，返回数字 `0` 或 `1`：

```js
const level = gpio.read(4);
```

输入引脚返回当前硬件电平。输出引脚返回 GPIO API 最近成功设置并保存的输出
电平。引脚尚未配置时，自动以 `{mode: "input", pull: "none", debounceMs: 20}`
配置为输入。输入模式下 `read()` 立即读取硬件电平，20ms 去抖只影响后续 `watch()`
的事件。已有配置不会被修改。

## `gpio.write(pin, value)`

设置输出引脚电平。`value` 可以是 `0`、`1`、`false` 或 `true`，成功时返回
`undefined`：

```js
gpio.write(2, 1);      // 高电平
gpio.write(2, true);   // 高电平
gpio.write(2, 0);      // 低电平
gpio.write(2, false);  // 低电平
```

目标引脚尚未配置时，自动以 `{mode: "output", initial: value}` 配置为输出，
先设置初始电平再启用输出，避免短暂输出相反电平。已配置为输出时直接写入；
已配置为输入时抛出 `InternalError`，不会自动切换模式。

## `gpio.watch(pin, edge, callback)`

监听输入引脚的电平边沿。未配置时自动使用
`{mode: "input", pull: "none", debounceMs: 20}` 配置为输入。已配置为输入时
保留现有上下拉和去抖设置；已配置为输出时抛出 `InternalError`。

```js
const unwatch = gpio.watch(4, "falling", event => {
  console.log(event.pin, event.value, event.edge);
});
```

`edge` 支持：

| 值 | 触发条件 |
| --- | --- |
| `"rising"` | 从低电平变为高电平 |
| `"falling"` | 从高电平变为低电平 |
| `"both"` | 任一方向的电平变化 |

回调收到的事件为：

```js
const event = {
  pin: 4,
  value: 0,
  edge: "falling",
  timestampUs: 1234567
};
```

`timestampUs` 是设备启动后的单调微秒时间，在底层中断时采集。启用去抖时，
只有最终稳定的边沿才会派发，时间戳仍对应触发这次稳定状态的最后一次跳变。
它不受 JavaScript 回调排队延迟或系统时间调整影响。

每次调用都会创建独立订阅，并返回一个可重复安全调用的取消函数：

```js
unwatch();
unwatch(); // 空操作
```

若监听需要跨远程 eval 调用保留取消入口，可显式保存到全局对象：

```js
globalThis.stopButtonWatch = gpio.watch(4, "both", event => {
  console.log("button", event.value);
});

// 稍后取消
globalThis.stopButtonWatch();
delete globalThis.stopButtonWatch;
```

同一引脚无论注册多少 watcher，底层只使用一个原生中断。回调按照注册顺序在
JavaScript EventLoop 任务中执行，而不是在中断上下文中执行。回调分发使用
监听器快照，因此在回调内新增、取消、重新配置或复位只会影响下一次事件。

`debounceMs` 是该引脚全部 watcher 共享的稳定态延迟。设置为非零值后，只有
输入电平保持该时间不再变化，才会派发最终边沿事件。

全局 watcher 数量由 `CONFIG_JS_GPIO_MAX_WATCHERS` 限制，默认最多 32 个。

## `gpio.reset(pin)`

删除该引脚的全部 watcher、停止防抖计时器，并将引脚恢复到默认状态：

```js
gpio.reset(4);
```

如果引脚此前已由 GPIO API 配置，返回 `true`；未配置时返回 `false`。Runtime
停止时也会自动复位所有由此 API 配置的 GPIO。

## 自动配置规则

`read()`、`write()` 和 `watch()` 只在引脚未配置时自动配置，所有调用参数校验
通过后才执行自动配置。引脚可用性检查与显式 `setup()` 相同；被保留、禁止
使用或不支持目标模式的引脚仍会报错。

显式 `setup()` 的默认值保持不变：输入 `pull: "none"`、`debounceMs: 0`，
输出 `initial: 0`。先调用 `read()` 或 `watch()` 会将引脚配置为输入，随后
`write()` 会报错，需要显式 `setup()` 切换为输出。`reset()` 后再次调用上述
接口会重新自动配置。

## 常见异常

GPIO API 使用 JavaScript 异常报告错误。常见情况包括：

| 情况 | 结果 |
| --- | --- |
| pin 不是整数或不是有效 GPIO | `TypeError` 或 `RangeError` |
| 引脚被保留或不在 allowed-pin mask 中 | `RangeError` |
| 不支持输出的引脚配置为输出 | `RangeError` |
| `mode` 缺失或配置字段类型错误 | `TypeError` |
| `mode`、`pull`、`debounceMs` 等值超出范围 | `RangeError` |
| 写入已配置为输入的引脚 | `InternalError` |
| 监听已配置为输出的引脚 | `InternalError` |
| watcher 达到全局上限 | `RangeError` |

底层 ESP-IDF GPIO 操作失败时会抛出 `InternalError`，消息中包含失败的操作和
ESP-IDF 错误名称。
