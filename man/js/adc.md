# ADC API

[返回索引](index.md)

启用 `CONFIG_JS_ADC_ENABLED`（ESP32-S3 默认启用）后提供全局 `adc` 对象。
接口以 GPIO 编号寻址，与 `gpio` 一致；无需 import。

```js
console.log(JSON.stringify(adc.list()));
adc.setup(1, {attenuation: 12});
console.log(adc.read(1));        // 12 位原始整数，0..4095
console.log(adc.readVoltage(1)); // 本次采样的校准电压，整数 mV
adc.reset(1);
```

## 接口约定

| 方法 | 返回值 | 行为 |
| --- | --- | --- |
| `adc.list()` | 数组 | 枚举 ADC1 引脚及占用状态，不分配驱动资源 |
| `adc.setup(pin, options?)` | `undefined` | 配置模拟输入；默认 `{attenuation: 12}` |
| `adc.read(pin)` | 整数 | 单次读取 12 位原始值 |
| `adc.readVoltage(pin)` | 整数 mV | 单次读取并使用 eFuse 曲线拟合校准 |
| `adc.reset(pin)` | boolean | 释放已配置引脚时为 true，未配置时为 false |

`list()` 每项包含 `pin`、`available`、`configured`、`attenuation`。
未配置时 `attenuation` 为 null；不可用时附带 `reason`，值为
`"not-allowed"`（不在固件允许掩码内）或 `"reserved"`（已被其他外设占用）。
已由本 ADC 模块配置的引脚仍标记为 available。

`pin` 必须是支持 ADC1 的整数 GPIO 编号；ESP32-S3 对应 GPIO 1..10，
实际可用性以板级允许掩码及占用状态为准。ADC2 引脚不在首版支持范围内。

`options` 仅接受数值 `attenuation`：`0`、`2.5`、`6`、`12`（dB）。
省略选项或传入 undefined 使用默认值；null、数组、函数、未知选项均报错。
分辨率固定 12 位。相同配置重复 setup 为幂等操作；改变衰减必须先 reset。
各衰减档的可测电压范围和精度受芯片限制，12 dB 不表示可以直接输入 12 V。

`read` 与 `readVoltage` 分别触发一次采样，并非同一个采样点的两种表示。
校准句柄在第一次 readVoltage 时创建并复用；若芯片缺少校准数据或驱动
失败则抛出异常，不返回估算电压。校准失败不影响后续原始值读取。

参数类型错误抛出 `TypeError`；无效 ADC1 引脚、禁用引脚及非法衰减值抛出
`RangeError`；引脚占用、未配置、需要 reset、驱动超时和校准失败抛出
`InternalError`。带有 getter 的选项对象所抛出的异常原样传播。
reset 不会释放其他外设占用的引脚。

## 采样方式与生命周期

首版定位为传感器低频单次采样，底层采用 ESP-IDF oneshot 驱动。
所有方法同步运行于 JS EventLoop，不创建后台任务，也不提供 DMA、连续采样
或 watch。需要周期读取时使用现有定时器：

```js
adc.setup(1);
const timer = setInterval(() => {
    console.log(adc.readVoltage(1));
}, 1000);
// 停止采样时：
// clearInterval(timer);
// adc.reset(1);
```

ADC1 单元在首次 setup 时分配，多个通道共享；释放最后一个通道时销毁。
引脚通过 ESP-IDF GPIO reservation 与现有 GPIO/I²C/UART 组件协作。
运行时退出会清理所有引脚、校准句柄和 ADC 单元。reset 将引脚恢复到
SDK 的 GPIO 默认复位状态。setup 失败会释放此次申请的资源和引脚占用，
不销毁其他通道正在使用的 ADC 单元。
