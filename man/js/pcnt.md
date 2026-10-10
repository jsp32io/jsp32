# PCNT API

[返回索引](index.md)

启用 `CONFIG_JS_PCNT_ENABLED` 后，全局对象 `pcnt` 使用 ESP32-S3 的硬件
脉冲计数器。输入边沿由硬件累计，JavaScript 读取计数时才参与。

## 单路脉冲

```js
const counter = pcnt.open({pin: 4, edge: "rising", pull: "up"});
counter.start();
console.log(counter.read());
counter.stop();
counter.close();
```

`edge` 可为 `"rising"`、`"falling"` 或 `"both"`，默认 `"rising"`。
`pull` 可为 `"none"`、`"up"` 或 `"down"`，默认 `"none"`。
`filterNs` 为非负整数，默认 0（关闭硬件毛刺过滤）。实际可用范围由驱动
检查；它不是机械按键的毫秒级去抖。

## 双路正交编码器

```js
const encoder = pcnt.openQuadrature({a: 4, b: 5, resolution: 4});
encoder.start();
console.log(encoder.read());
encoder.close();
```

`a` 和 `b` 必须是不同的 GPIO。`resolution` 取 `1`、`2`、`4`，默认
`4`，表示每个完整 A/B 信号周期计入的变化次数。交换 A、B 会反转
计数方向。`pull` 同时应用于两根引脚；`filterNs` 与单路模式一致。

## 计数器方法

| 方法 | 行为 |
| --- | --- |
| `start()` | 开始计数；重复调用无影响 |
| `stop()` | 停止计数，保留读数；重复调用无影响 |
| `read()` | 返回有符号 32 位累计计数 |
| `clear()` | 清零当前计数 |
| `sample()` | 返回 `{count, timestampUs}`，时间为读取后取得的单调微秒时间 |
| `close()` | 释放引脚和硬件资源；重复调用无影响 |

计数器打开后处于停止状态。关闭后除 `close()` 外的方法都会报错。
运行时退出会自动关闭所有计数器。引脚须符合板级允许掩码且未被其他
外设占用；没有可用 PCNT 单元时 `open` 会报错。硬件原生计数宽度最多
16 位；驱动启用溢出补偿后 API 提供有符号 32 位读数。极高频率下
补偿中断仍可能来不及处理，不能保证任意频率均无漏计。

频率可用两次 `sample()` 的计数差和实际时间差计算。单路 `"both"`
在理想方波中每周期计两次；`"rising"` 和 `"falling"` 每周期各计一次。

设计依据：[ESP-IDF PCNT 文档](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/pcnt.html)。
