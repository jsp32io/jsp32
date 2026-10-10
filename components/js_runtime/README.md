# QuickJS Runtime 与 EventLoop

该组件让一个专属 FreeRTOS 任务拥有 QuickJS Runtime 和全部 `JSValue`。其他任务及 ISR 只通过 Queue 投递纯 C 事件，不能直接调用 QuickJS。

## LittleFS ES 模块

`/main.js` 作为 ES 模块执行，可从 LittleFS 导入 `.js` 文件。裸名称从
`/stdlib` 查找，前缀和 `.js` 后缀均可省略；相对路径从当前模块所在目录
解析，绝对路径从文件系统根目录解析：

```js
import { add } from "math";            // /stdlib/math.js
import { add as same } from "math.js";  // 同一个模块
import { helper } from "./lib/helper.js";
import "/shared/bootstrap.js";
import { sleep } from "time";          // /stdlib/time.js
```

路径会规范化，同一文件只初始化一次。相对路径及绝对路径须显式使用 `.js`；
越过根目录、URL、npm 包解析及非 `.js` 文件均不支持。导入缺失、路径非法或
源码超限时会在串口记录异常。单文件默认上限 64 KiB，单次 Runtime 默认
最多加载 64 个文件模块、累计 512 KiB 源码，均可通过 menuconfig 调整。

模块变更在重启后生效。部署多文件时，先上传依赖模块，最后上传 `/main.js`，
然后重启；文件系统只保证每个文件的原子替换。HTTP `eval` 保持现有脚本
求值语义。`/main.js` 改为模块后，顶层 `var` 不再写入全局对象，未声明变量
赋值会报错；需要共享全局状态时显式使用 `globalThis`。

## JavaScript API

```js
const timeout = setTimeout(callback, delayMs, ...args);
clearTimeout(timeout);

const interval = setInterval(callback, intervalMs, ...args);
clearInterval(interval);

events.on("message", callback);
events.off("message", callback); // 省略 callback 时删除全部 message 监听器
events.post("message");
events.stats();                  // { queued, dropped }

gpio.list();
gpio.setup(pin, options);
gpio.read(pin);
gpio.write(pin, value);
gpio.watch(pin, edge, callback); // 返回取消监听函数
gpio.reset(pin);

const counter = pcnt.open({pin: 4, edge: "rising"});
counter.start();
counter.read();
counter.close();

uart.list();
const port = uart.open(1, {tx: 17, rx: 18, baudRate: 115200});
await port.write("AT\r\n");
const data = await port.read(256, 1000);
port.close();

const bus = i2c.open(0, {sda: 8, scl: 9});
const device = bus.device(0x76, {clockHz: 400000});
const value = await device.writeRead(new Uint8Array([0xd0]), 2);
await bus.close();

registers.writeWords("D100", new Uint16Array([10, 20, 30]));
const words = registers.readWords("D100", 3);

const conn = await net.dialTCP({host: "192.168.1.100", port: 9000});
await conn.write(new Uint8Array([1, 2, 3]));
const bytes = await conn.read(4096, {timeoutMs: 3000});
await conn.close();
```

Timer 回调和事件监听器执行后，EventLoop 会处理 Promise Job。单轮 Job 与 Timer 数量均有限制，剩余工作会在下一轮继续。

GPIO 由外部 `js_gpio` 组件通过模块接口安装，完整参数、引脚策略和去抖语义见
[JavaScript GPIO API](../js_gpio/README.md)。

UART 由外部 `js_uart` 组件安装。读写操作在原生工作任务中执行，通过 Promise
返回结果，不会阻塞 EventLoop。完整配置和关闭语义见
[JavaScript UART API](../js_uart/README.md)。

I²C 由外部 `js_i2c` 组件安装。每条 bus 使用独立 worker 执行有界
FIFO 中的阻塞传输，完整语义见
[JavaScript I²C API](../js_i2c/README.md)。

PLC 风格的内存寄存器由外部 `js_registers` 组件安装，提供
`X/Y/M` bit 区和 `D` 16-bit word 区。完整语义见
[JavaScript Registers API](../js_registers/README.md)。

TCP/TLS 由外部 `js_net` 组件安装，提供 Promise 风格的连接、TCP 监听、
双向字节流和半关闭。完整参数、证书验证、背压和超时语义见
[JavaScript TCP/TLS API](../js_net/README.md)。

`console.log()` 同时写入串口和一个有界历史缓冲。其他组件可以通过
`js_runtime_console_subscribe()` 订阅最近及后续的日志；慢消费者发生丢弃时，
`js_runtime_console_receive()` 会返回随下一条消息携带的丢弃数量，或在队列清空后
用 `ESP_ERR_NOT_FOUND` 单独报告丢弃数量。

其他任务可以通过 `js_runtime_eval_submit()` 提交 `expr` 或 `script` 求值请求，
再用 `js_runtime_eval_receive()` 等待纯 C 结果。源码在提交时复制，所有 QuickJS
调用和 `JSValue` 仍只出现在 EventLoop 任务。若结果是 Promise，EventLoop 会在每轮
任务后检查其状态，不会阻塞 Timer、事件或其他 Promise Job。调用方超时或断开后可
直接销毁请求；引用计数保证队列事件或待决 Promise 完成前对象仍然有效。

## 添加外设组件

外设组件先注册一个初始化函数：

```c
static esp_err_t gpio_js_init(js_runtime_t *runtime,
                              JSContext *ctx, void *opaque)
{
    /* 注册全局 gpio 对象和 JS_EVENT_GPIO 处理器。 */
    return js_event_register_handler(runtime, JS_EVENT_GPIO,
                                     gpio_event_handler, opaque);
}

static void gpio_js_deinit(js_runtime_t *runtime,
                           JSContext *ctx, void *opaque)
{
    /* 注销 ESP-IDF 回调，停止驱动并释放模块资源。 */
}

void app_main(void)
{
    ESP_ERROR_CHECK(js_runtime_register_module(
        gpio_js_init, gpio_js_deinit, NULL));
    ESP_ERROR_CHECK(js_runtime_start(script, script_size, "/main.js",
                                     filesystem, &runtime));
}
```

初始化和清理函数都由 EventLoop 任务调用，因此可以安全创建或释放 JavaScript 对象、注册事件处理器和监听器。模块按注册顺序初始化、按相反顺序清理。最多可注册 12 个模块，且必须在 `js_runtime_start()` 前完成。

普通任务投递事件：

```c
js_event_t event = {
    .type = JS_EVENT_GPIO,
    .subtype = GPIO_EVENT_RISING,
    .source = pin,
    .payload = payload,
    .payload_size = sizeof(*payload),
    .free_payload = free,
};
esp_err_t err = js_event_post(runtime, &event, 0);
```

`js_event_post()` 无论成功还是失败都会取得 payload 所有权。事件处理器返回后，框架调用 `free_payload`。EventLoop 任务自身投递事件时必须使用零等待，避免队列已满时等待自己消费队列。

ISR 只能投递不含 payload 的固定字段事件：

```c
BaseType_t wake = pdFALSE;
js_event_t event = {
    .type = JS_EVENT_GPIO,
    .subtype = GPIO_EVENT_RISING,
    .source = pin,
};
js_event_post_from_isr(runtime, &event, &wake);
portYIELD_FROM_ISR(wake);
```

需要动态数据的 ISR 应先投递固定标识，再由外设工作任务读取或复制数据。

## 通用 Promise 请求

`js_request.h` 为原生异步模块提供公共请求表，I²C、UART 和 TCP/TLS 已复用它。
请求表只由 EventLoop 任务访问，worker 仍只持有原生数据、请求 ID 和 generation。
每条总线或端口可以拥有独立请求表；ID 不为零，回绕时跳过仍在使用的 ID，
完成时同时匹配 ID 与 generation，避免旧设备实例的结果完成新请求。

模块初始化时建立请求表并注册到 Runtime：

```c
js_request_table_init(&module->requests, ctx);
js_runtime_track_requests(runtime, &module->requests);

uint32_t id;
JSValue promise = js_request_create(&module->requests, generation,
    metadata, free, cancel_native_operation, &id);
/* 成功后投递只包含原生数据及 id/generation 的工作命令。 */
```

`js_request_create()` 返回 Promise，失败时返回 `JS_EXCEPTION`、将 ID 置零，
不接管 metadata。成功后，完成、取消或关闭请求表会调用可选的释放函数。
完成事件通过 `js_request_resolve/reject()` 传入借用的结果或错误值，调用方
仍负责释放该值以及原生 completion payload。找不到匹配请求时返回 `false`，
迟到结果不能再次完成 Promise。完成前先从表中移除请求，再调用 Promise
settler、释放保存的 JS 回调和 metadata。

`js_request_cancel()` 先移除请求，再执行可选的原生取消钩子，最后 reject
并释放请求资源。取消钩子运行在 EventLoop 中，应有界且不能等待 EventLoop
自身处理事件。如果 worker 仍在访问原生数据，必须保留那部分数据至 worker
确认结束；可使用独立所有权或不提供 metadata 释放钩子。TCP/TLS 的 worker
请求对象仍由模块在原生完成或 worker 停止后释放。

可通过 `js_request_set_deadline()` 设置单调时钟的微秒截止时间，零表示无
公共截止时间。已注册的表由 Runtime 自动检查，到期以 `TimeoutError` reject
并调用取消钩子；最近截止时间参与 Queue 阻塞等待，不增加空闲轮询。
I²C、UART 和 TCP/TLS 保留原有的原生超时规则及错误类型，不额外设置公共
截止时间。`js_request_expire()` 和 `js_request_next_deadline()` 也可供独立
调度器使用；单次过期处理有数量上限，防止取消钩子不断创建过期请求。

Runtime 停止时先让外设模块停止 worker 和清理自己的请求，再关闭所有已
注册的表作为兜底。关闭的表拒绝新请求。表的存储必须保持有效至 Runtime
清理结束，注册后不能重新初始化；设备关闭再打开时保留请求表，更新 generation。
请求表的 `count` 表示尚未完成的 Promise；取消后的原生操作若仍在进行，
应由模块单独计入驱动资源与背压上限。

主机测试：`python3 tests/js_request_host/run.py`；支持
`SANITIZE=address,undefined` 检查释放路径和内存错误。

## 配置

`idf.py menuconfig` 的 `QuickJS runtime` 菜单可以配置：

- Queue 长度和 EventLoop 任务栈、优先级。
- QuickJS 堆与 JavaScript 栈上限。
- 单次 JavaScript 调用的时间预算。
- 每轮 Promise Job 和到期 Timer 上限。

## 生命周期

`js_runtime_start()` 等待 Runtime、Context、内建 API 和外部模块完成初始化后才返回。`js_runtime_stop()` 投递停止事件；停止流程会拒绝新事件，等待正在投递的生产者退出，释放 Queue 及其中的 payload、Timer、监听器、事件处理器、Context 和 Runtime。

当前实现是单 Runtime、单次启动模型；停止后不支持再次启动。
