# 定时器 API

[返回索引](index.md)

## 接口

| 函数 | 返回值 | 行为 |
| --- | --- | --- |
| `setTimeout(callback, delayMs?, ...args)` | 数字 ID | 延迟后调用一次 |
| `setInterval(callback, intervalMs?, ...args)` | 数字 ID | 周期调用 |
| `clearTimeout(id)` | `undefined` | 取消 ID 对应的定时器 |
| `clearInterval(id)` | `undefined` | 取消 ID 对应的定时器 |
| `monotonicMs()` | 数字 | 设备启动后的单调毫秒时间 |

```js
const once = setTimeout(value => console.log(value), 500, "ready");
// clearTimeout(once);

let ticks = 0;
const interval = setInterval(() => {
  console.log(++ticks);
  if (ticks === 3) clearInterval(interval);
}, 1000);
```

callback 必须是函数，否则抛出 `TypeError`，不接受代码字符串。
额外参数按顺序传入回调。

延时按 QuickJS 整数转换处理；省略时为 0，负值按 0 处理，超过
2147483647 ms 的值限制为该上限。setInterval 的最小间隔为 1 ms。
取消不存在或已取消的 ID 不产生效果；两个 clear 函数使用同一套 ID。

## 调度语义

定时器回调在 EventLoop 中执行。零延时表示尽快调度，不会在 setTimeout
调用过程中同步执行。时间参数不是精确触发承诺，其他 JS 工作和系统调度会
推迟执行。

周期任务落后时会跳过错过的周期，避免连续补执行。每轮可执行的到期定时器
数量由 `CONFIG_JS_RUNTIME_MAX_TIMERS_PER_TURN` 控制，默认 32。
Runtime 退出会清理所有定时器。

`monotonicMs()` 与 GPIO 事件的 `timestampUs` 使用同一个时钟，可用于计算
持续时间；它不受系统日期或网络校时影响。两者单位分别为毫秒和微秒。

调度器不等待 async 回调返回的 Promise；若一次异步工作可能超过间隔，
使用 `await` 和 [sleep](time.md) 串行执行，避免产生多个未完成操作：

```js
import {sleep} from "time";

async function poll(device) {
  for (;;) {
    const bytes = await device.read(2);
    console.log(bytes);
    await sleep(1000);
  }
}
```
