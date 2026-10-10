# 模块与运行环境

[返回索引](index.md)

## 入口与导入

LittleFS 中的 `/main.js` 作为 ES 模块执行，支持 `import`、`export` 和
顶层 `await`。可使用标准 JavaScript 对象、Promise、ArrayBuffer 和类型化数组。
固件外设对象直接位于全局环境，标准库通过 import 加载。

```js
import {sleep} from "time";             // /stdlib/time.js
import {createSlave} from "modbus.js";  // /stdlib/modbus.js
import {helper} from "./lib/helper.js"; // 相对于当前模块
import "/shared/bootstrap.js";         // LittleFS 根目录

await sleep(100);
console.log("ready");
```

| 路径形式 | 解析规则 |
| --- | --- |
| `"time"`、`"time.js"` | `/stdlib/time.js` |
| `"./lib/helper.js"`、`"../helper.js"` | 相对于当前模块目录 |
| `"/shared/bootstrap.js"` | 相对于 LittleFS 根目录 |

相对路径和绝对路径必须包含 `.js` 后缀。规范化后指向同一文件的路径只加载
一次。越过根目录、URL、npm 包解析及非 `.js` 文件不受支持。

模块顶层 `var` 不会写入全局对象，未声明变量赋值会报错。跨模块数据优先
使用 export/import；确需全局共享时显式写入 `globalThis`。

## 异步执行

所有 JavaScript 在同一 EventLoop 中执行。UART 和 I²C I/O 返回 Promise，
调用方使用 `await` 或 `.then/.catch` 取得结果和处理错误。
定时器、消息及外设监听回调也在此 EventLoop 执行。

```js
const bus = i2c.open(0, {sda: 8, scl: 9});
try {
  const sensor = bus.device(0x76);
  console.log(await sensor.read(2));
} catch (error) {
  console.log(String(error));
} finally {
  await bus.close();
}
```

同步长循环会阻塞其他回调，固件还会限制一次 JS 调用的时间预算。不要用
忙等待实现延时，使用 [time.sleep](time.md) 或 [定时器](timers.md)。

## 部署与限制

模块修改在重启后生效。多文件部署时先上传依赖，最后上传 `/main.js`，
然后重启。文件系统保证单文件原子替换，不保证多个文件一起切换。
HTTP eval 使用脚本求值语义，不等同于 `/main.js` 的模块执行语义。

默认限制：单个模块源码 64 KiB，每次 Runtime 最多 64 个文件模块，累计
源码 512 KiB；可通过 menuconfig 的 QuickJS runtime 配置调整。
堆、栈、单次调用预算和每轮 Promise Job 数量也受配置限制。

当前固件不提供浏览器 DOM、`fetch`、Node.js API、QuickJS 桌面 `std/os`
模块或 npm 包解析。JavaScript 可通过 [Filesystem](fs.md) 管理 LittleFS 文件；
固件也通过原生文件系统加载脚本。
Runtime 停止时清理外设、定时器和监听器；当前原生 Runtime 为单次启动模型，
重新运行应用需重启固件。
