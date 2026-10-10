# Time 标准库

[返回索引](index.md)

## `sleep(milliseconds)`

```js
import {sleep} from "time";

console.log("start");
await sleep(1000);
console.log("after one second");
```

返回 `Promise<void>`，到期后以 undefined 完成，不阻塞 EventLoop。
模块位于 LittleFS `/stdlib/time.js`，也可通过 `"time.js"` 导入。

sleep 基于 `setTimeout`，参数转换、范围与调度精度遵循
[定时器 API](timers.md)。它不提供取消句柄；需要取消时直接使用 setTimeout
和 clearTimeout。设置定时器失败时 Promise 会被拒绝。

循环工作中可以用 sleep 让出执行机会：

```js
import {sleep} from "time";
adc.setup(1);
for (let i = 0; i < 10; ++i) {
  console.log(adc.readVoltage(1));
  await sleep(1000);
}
adc.reset(1);
```
