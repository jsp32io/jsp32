# JavaScript API 索引

[手册总索引](../index.md)

JSP32 使用 QuickJS 执行 LittleFS 中的 `/main.js`。本文档按 API 分类，
介绍当前固件提供的接口、参数、返回值、错误和用法。

在设备命令行客户端中执行 `cli man js` 查看本索引，执行 `cli man js.gpio`
查看 GPIO 手册，其他页面同样使用 `js.<文件名>`。HTTP 可直接访问
`GET /api/v1/man/js` 或 `GET /api/v1/man/js.gpio`，返回原始 Markdown。

## 运行环境

| 文档 | 内容 | 使用方式 |
| --- | --- | --- |
| [模块与运行环境](modules.md) | 入口脚本、import 路径、Promise、运行限制 | ES 模块 |
| [Console](console.md) | `console.log` 日志输出 | 全局 `console` |
| [定时器](timers.md) | `setTimeout`、`setInterval`、`monotonicMs` 及取消 | 全局函数 |
| [Events](events.md) | 消息订阅、投递、取消和队列状态 | 全局 `events` |
| [Filesystem](fs.md) | LittleFS 目录、文件读写与删除 | 全局 `fs` |
| [开发和部署插件](plugins.md) | 插件格式、依赖声明与上传步骤 | `/plugins/<名称>/index.js` |
| [遥测数据](telemetry.md) | 插件遥测字段、发送规则和当前 MQTT 上报方式 | 插件 `telemetry` 声明 |
| [io 插件](plugins/io.md) | DI 输入监测、寄存器映射与编码器计长 | `/plugins/io/index.js` |

## 外设与寄存器

| 文档 | 内容 | 启用配置 |
| --- | --- | --- |
| [GPIO](gpio.md) | 数字输入输出、上下拉、边沿监听、去抖 | `CONFIG_JS_GPIO_ENABLED` |
| [PCNT](pcnt.md) | 硬件脉冲计数、双路正交编码器 | `CONFIG_JS_PCNT_ENABLED` |
| [ADC](adc.md) | ADC1 原始值、衰减、校准毫伏读取 | `CONFIG_JS_ADC_ENABLED` |
| [UART](uart.md) | 串口异步读写、分帧、半双工方向控制 | `CONFIG_JS_UART_ENABLED` |
| [I²C](i2c.md) | 主机总线、设备、探测、异步事务、`I2CError` | `CONFIG_JS_I2C_ENABLED` |
| [寄存器使用手册](registers.md) | X/Y/M/D 地址、读写、监控和变更监听 | `CONFIG_JS_REGISTERS_ENABLED` |

这些 API 直接通过全局 `gpio`、`pcnt`、`adc`、`uart`、`i2c`、`registers` 使用，
不需要 import。实际可用引脚和控制器由芯片、板级配置及当前占用状态决定，
示例中的引脚编号需按接线调整。

## 网络

| 文档 | 内容 | 启用配置 |
| --- | --- | --- |
| [Net：TCP 与 TLS](net.md) | TCP 监听、异步读写；TLS 客户端、系统及自定义 CA | `CONFIG_JS_NET_ENABLED` / `CONFIG_JS_NET_TLS_ENABLED` |

## 标准库

| 文档 | 导出接口 | 导入方式 |
| --- | --- | --- |
| [Time](time.md) | `sleep(milliseconds)` | `import {sleep} from "time"` |
| [VL53L0X](vl53l0x.md) | `VL53L0X` 测距驱动 | `import {VL53L0X} from "vl53l0x"` |
| [Modbus](modbus.md) | `createSlave(options)`、`crc16(bytes)` | `import {createSlave, crc16} from "modbus"` |
| [Smux v1](stdlib/smux.md) | `client(transport)`、`server(transport)`、`Session` | `import {client, server} from "smux"` |

标准库文件随 LittleFS 镜像部署在 `/stdlib`，依赖文件需要存在才能导入。
本文档聚焦脚本可调用的 API；HTTP 管理与远程 eval 接口见
[JS HTTP 组件](../../components/js_http/README.md)，原生扩展方式见
[Runtime 组件](../../components/js_runtime/README.md)。

## 异步读取约定

以下接口的最后一个参数均为可选的 `{timeoutMs?: number}` 对象。
`timeoutMs` 为非负整数，0 表示无限等待；等待超时通过 Promise reject
报告，错误的 `code` 为 `"TIMEOUT"`。未知 options 字段和数字 options
不接受。各模块仍保留自身的数据格式、长度限制及默认超时。

| 接口 | 默认超时 | 成功返回值 |
| --- | --- | --- |
| `port.read(maxBytes, options?)` | 0 | `Uint8Array`，最多 maxBytes 字节 |
| `port.readFrame(options?)` | 0 | `Uint8Array`，完整一帧 |
| `conn.read(maxBytes, options?)` | 0 | `Uint8Array`，EOF 为 null |
| `device.read(length, options?)` | 1000 ms | `Uint8Array`，恰好 length 字节 |
| `device.writeRead(data, readLength, options?)` | 1000 ms | `Uint8Array`，恰好 readLength 字节 |
| `stream.read(maxBytes = 32768, options?)` | 0 | `Uint8Array`，流结束为空数组 |
| `sensor.read(options?)` | 实例 timeoutMs，默认 1000 ms | `{distanceMm, rawStatus, valid}` |
| `sensor.readRangeMillimeters(options?)` | 实例 timeoutMs，默认 1000 ms | 有效距离，整数 mm |

有限读取期限包含排队时间；UART/I²C 的排队请求在 worker 取出时检查期限，
前面的未完成事务仍按顺序执行。读取超时不会主动关闭端口、连接或流；
UART 部分帧会丢弃以重新同步。I²C 即使无限等待，硬件超时和 NACK
仍可使事务结束。传感器无效测量结果与等待超时不同，详见对应手册。
