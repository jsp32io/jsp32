# VL53L0X 测距库

库文件 `/stdlib/vl53l0x.js`，使用全局 `i2c`、`gpio` 和定时器。

```js
import {VL53L0X} from "vl53l0x";

const tof = new VL53L0X({busId: 1, sda: 5, scl: 6, xshut: 7});
await tof.init();
await tof.setAddress(0x30); // 7-bit 地址，掉电或 XSHUT 复位后恢复 0x29
console.log(await tof.readRangeMillimeters()); // 有效距离，单位 mm
console.log(await tof.read()); // {distanceMm, rawStatus, valid}
await tof.startContinuous(100); // 每 100ms 一次；0 为连续背靠背模式
console.log(await tof.read());
await tof.stopContinuous();
await tof.close();
```

默认配置对应 SDA=GPIO5、SCL=GPIO6、XSHUT=GPIO7，使用 I²C1，
避免占用小车使用的 I²C0。构造函数不访问硬件，`init()` 才申请资源。
默认实例独占所选总线；`close()` 释放总线和 XSHUT。关闭后需创建新实例。
传入 `bus` 时复用已打开的总线，此时 `close()` 仅关闭自己的设备句柄，
不会关闭共享总线。若设置 `xshut:null`，XSHUT 由调用者统一管理。
GPIO 初始化会检查 XSHUT 是否已被配置，避免覆盖其他用途。

| 构造选项 | 默认值 | 说明 |
| --- | --- | --- |
| `busId` | `1` | 独立的空闲 I²C 控制器 |
| `bus` | `null` | 已打开的共享 I²C 总线；提供后忽略 busId/sda/scl/pullups |
| `sda` / `scl` / `xshut` | `5` / `6` / `7` | `xshut:null` 不控制关断脚 |
| `address` | `0x29` | 7-bit 地址；XSHUT 复位会恢复为此默认地址 |
| `clockHz` | `100000` | I²C 时钟 |
| `pullups` | `"external"` | 可设 `"internal"` 调试；硬件应有外部上拉 |
| `timeoutMs` | `1000` | 每次总线传输及每段轮询等待的超时，1..60000ms |
| `timingBudgetUs` | `33000` | 单次测量时间预算，20000..1000000µs，必须小于超时 |
| `io2v8` | `true` | 启用传感器 2.8V I/O 模式 |

`init()` 包含型号检查、SPAD 设置、默认调谐、时间预算和参考校准；
失败时释放资源，可重试。各次公开操作按调用顺序串行执行。
连续模式周期必须为 0，或不小于测量预算且小于 `timeoutMs`，最大 60000ms。
轮询会异步让出事件循环。超时抛出 `code="TIMEOUT"`；原生 I²C 错误直接传递。
整个 `init()` 或 `read()` 可能包含多段等待，`timeoutMs` 不是整个方法的总截止时间。

`rawStatus` 是硬件状态码，**不是 ST API 的 RangeStatus 枚举**。
库将硬件状态 11 且距离小于 8190mm 标为有效；没有实现 ST API 的完整
sigma/串扰质量检查，也不含加盖玻璃后的校准。
`read()` 保留无效结果供诊断，`readRangeMillimeters()` 遇无效结果抛出
`code="INVALID_RANGE"`，原始结果位于 `error.result`。有效标记不替代实际距离精度标定。

部署与调用：

```sh
JSP32_BASE_URL=http://192.168.3.4 ./cli/cli fs put /stdlib/vl53l0x.js rootfs/stdlib/vl53l0x.js
JSP32_BASE_URL=http://192.168.3.4 ./cli/cli eval '(async () => { const {VL53L0X} = await import("vl53l0x"); globalThis.tof = new VL53L0X(); await tof.init(); return await tof.readRangeMillimeters(); })()'
JSP32_BASE_URL=http://192.168.3.4 ./cli/cli eval 'tof.readRangeMillimeters()'
```

也可将导入和初始化加入 `/main.js` 以开机加载。完整有限次采样示例位于
`rootfs/examples/vl53l0x.js`。同一运行期模块被缓存，更新已导入库后需重启加载。

初始化和寄存器算法改编自 [Pololu VL53L0X 驱动](https://github.com/pololu/vl53l0x-arduino)，
Pololu 和 ST 的许可声明已内嵌库文件。

## 多颗传感器和启动分配地址

`await sensor.setAddress(address)` 接受 7-bit 地址 `0x08..0x77`，无需左移。
`sensor.address` 返回当前句柄地址。改址前必须停止连续测距；库检查新地址
是否被占用，写入地址寄存器后替换设备句柄，并从新地址校验型号。
新地址已应答时抛出 `ADDRESS_IN_USE`；有同地址句柄时原生 API 拒绝申请。
改址写入/校验失败可能使硬件地址不确定，实例会关闭；调用者应通过 XSHUT
复位后创建新实例。地址不写入非易失存储，每次启动都要重新分配。

当前设备入口对应 `cli/main.js`，其中配置为：

| XSHUT | 地址 | 当前启用 |
| --- | --- | --- |
| GPIO7 | `0x30` | 是 |
| GPIO15 | `0x31` | 是 |
| GPIO16 | `0x32` | 否，待接线 |

三颗共用 I²C1 的 GPIO5/SDA、GPIO6/SCL。接入后把对应 `tofConfig` 项的
`enabled` 改为 `true`，上传 main.js 并重启；未启用项不配置其 GPIO。
所有实际接入的 TOF 都必须列入启用配置，不能让未管理的器件同时响应 `0x29`。
入口先将所有启用项 XSHUT 拉低，再逐颗拉高、初始化、改址，成功后才唤醒
下一颗。此顺序依据 [ST AN4846](https://www.st.com/resource/en/application_note/an4846-using-multiple-vl53l0x-in-a-single-design-stmicroelectronics.pdf)。

启动成功后 `globalThis.tofs` 是按启用配置顺序排列的实例数组，
`globalThis.tof` 指向第一颗。远程读取可用 `tof.readRangeMillimeters()`。
入口统一管理 XSHUT 和共享总线；关闭单颗实例不会拉低 XSHUT，也不会关闭
其它实例的总线。不要在运行中单独复位模块，否则其地址会恢复 `0x29`。
初始化失败时，入口关闭设备和总线，并保持已管理的 XSHUT 为低。
