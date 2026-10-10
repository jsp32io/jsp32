# 遥测数据

[JavaScript API 索引](index.md) · [插件机制](plugins.md)

MQTT 是 JSP32 原生服务，使用一个 broker 配置，不需要安装 MQTT 插件。
在内置 UI 的 `/mqtt` 页面配置地址和认证，保存后重启设备生效。默认关闭。
支持 `mqtt://` 和 `mqtts://`，TLS 使用固件内置的公共 CA 证书包；
私有 CA 需要定制固件证书包。设备 ID 与设备发现服务一致，为 STA MAC 的
12 位大写十六进制字符串。

## 插件声明

在插件默认导出对象中添加 `telemetry`：

```js
export default {
  telemetry: {
    mode: "interval",
    intervalMs: 1000,
    fields: {
      running: {address: "X0", type: "boolean"},
      risingCount: {address: "D108", type: "uint32"},
      lengthMm: {
        address: "D156", type: "int32",
        wordOrder: "highFirst", scale: 0.01,
      },
    },
  },
  setup({onCleanup}) {
    // 初始化业务资源；仍需登记自己的清理函数。
  },
};
```

插件启动成功后，加载器把声明注册到原生服务。声明非法或注册失败时，插件
启动失败并清理已申请的资源。停止插件时先注销上报，再清理业务资源；JS
运行时退出时，原生服务会清除所有 JS 上报定义。MQTT 连接由固件独立维护。
可用 `plugins.getTelemetry(id)` 查看声明的副本。

内置 UI 的“插件”页为声明了 `telemetry` 的插件提供“遥测数据”表单。
可调整启用状态、topic、发送模式与频率、QoS、retain，并编辑显示名、JSON 字段键、地址、
类型、字序、倍率和偏移。表格中的字段都会上报，删除某行即可移除该字段。
删除全部字段时不发送消息。可添加自定义字段或恢复插件默认。
保存前使用原生解析器校验启用的字段，保存后重启设备生效。
字段表提供“实时值”列，每秒只读刷新一次，按照当前表单中的类型、字序、
倍率和偏移解码，便于预览尚未保存的配置。
两字字段在同一次读取事务中获取；无效配置或读取失败会显示提示。
页面进入后台或离开插件页时暂停刷新，不改变表单或设备寄存器。

设置写入 `/plugins/<id>/telemetry.json`，与业务 `config.json` 分开，文件
不存在时使用插件声明；文件存在时完全使用文件配置，不与插件默认声明合并。
UI 保存完整配置，“恢复插件默认上报”删除该文件。字段必须提供地址和类型，
例如只上报电平和速度：

```json
{
  "mode": "change",
  "minIntervalMs": 200,
  "heartbeatMs": 30000,
  "fields": {
    "x1Level": {"address": "X1", "type": "boolean"},
    "x1SpeedHz": {"address": "D132", "type": "uint32", "scale": 0.000001}
  }
}
```

`fields: {}` 表示不读取、不发送任何字段。字段定义不支持 `disabled` 属性。
文件中的 `{}` 或格式错误会阻止插件启动，不会静默回退到插件默认配置。
省略可选参数时使用原生接口默认值，不继承插件中的对应参数。
重新打开页面时只显示已保存的字段，不会自动补回删除的字段。恢复默认会
重新使用插件声明的全部字段。
将上报设置中的 `enabled` 设为 `false` 可关闭该插件全部上报，业务插件继续运行。
`topic: ""` 表示使用 `/jsp32/<设备ID>/<插件ID>` 默认路径。
可通过 `plugins.getTelemetryConfig(id)` 查看默认声明与生效设置，通过
`plugins.getPendingChanges(id)` 查看尚未重启生效的上报设置。

| 属性 | 默认值 | 说明 |
| --- | --- | --- |
| `topic` | `/jsp32/<设备ID>/<插件ID>` | 完整发布 topic，1～256 个 UTF-8 字节，禁止 `+`、`#` |
| `mode` | `"interval"` | `"interval"` 或 `"change"` |
| `intervalMs` | 1000 | 定时模式的发送间隔，10～86400000 毫秒 |
| `minIntervalMs` | 200 | 变化模式的最短发送间隔，10～86400000 毫秒 |
| `heartbeatMs` | 10000 | 变化模式的完整状态心跳，不能小于 `minIntervalMs` |
| `qos` | 0 | 0、1 或 2 |
| `retain` | false | 是否保留 broker 上的最后一条消息 |
| `fields` | 必填 | 0～64 个具名字段，空对象表示不上报；字段名由字母开头，只含字母、数字、下划线，最长 63 字节 |

定时模式只使用 `intervalMs`；变化模式只使用 `minIntervalMs` 和 `heartbeatMs`。
混用两种模式的时间参数、未知属性或重复属性会被拒绝。调度精度受 10 ms
原生任务轮询及任务负载影响。每次发送都是所有声明字段的完整状态。

变化模式直接订阅原生寄存器变化，关注的任意字发生变化都会触发上报，并
在最短间隔内合并为最新状态。这是状态采样，不保证把每次边沿逐条发送。
需要记录脉冲数量时，应上报累计计数寄存器。

## 字段解码

字段可选 `name` 作为界面显示名，支持中文，最多 128 个 UTF-8 字节；省略时
显示 JSON 字段键。`name` 不改变上报数据的键，也不包含在上报 JSON 中。例如：

```js
fields: {
  lengthMm: {name: "编码器位移", address: "D156", type: "int32", scale: 0.01},
}
```

界面展示“编码器位移”，上报仍为 `{"data":{"lengthMm":123.45}}`。

字段可指定 `trigger: false`，让它的变化不触发变化上报；省略时默认 `true`。
这只影响触发条件，其他字段触发或心跳同步时仍发送该字段的最新值。
两字字段的任意一个字变化都按同一触发设置处理。所有字段都不触发时，
仍在连接建立、重连和心跳时发送完整状态。定时模式忽略此属性。
UI 的“变化触发”复选框对应 `trigger`；取消勾选不会移除上报数据。

| `type` | 寄存器 | 字数 |
| --- | --- | --- |
| `boolean` | X、Y、M | 1 位 |
| `uint16` / `int16` | D | 1 字 |
| `uint32` / `int32` | D | 2 个连续字 |
| `float32` | D | 2 个连续字，IEEE 754 |

两字类型可指定 `wordOrder: "highFirst"`（默认）或 `"lowFirst"`。
数值字段可指定 `scale`（默认 1）、`offset`（默认 0）：

```text
上报值 = 解码值 × scale + offset
```

地址必须处于实际容量内，两字类型的第二个字也必须有效。位字段不能指定
倍率或偏移。一条报告读取的字数不能超过原生寄存器事务上限。所有字段
通过一次原生快照读取；数据生产者也应使用一个事务更新需要一致的相关字。
同一寄存器可以用于多个字段。非有限浮点数或变换结果输出 JSON `null`。

默认 topic 下的消息示例：

```json
{"uptimeMs":123456,"data":{"running":true,"risingCount":1024,"lengthMm":123.45}}
```

`uptimeMs` 是设备本次启动以来的单调运行时间，不是 UTC 时间。每次采样和
JSON 编码只执行一次，然后发送到设备配置的 broker。单条 JSON 不超过
8192 字节，最多注册 32 条插件报告；每份声明序列化后最多 16 KiB。

ESP-MQTT 发送队列容量限制为 32 KiB。队列满时保留报告的待发送状态，
1 秒后重新采样重试；断线期间暂停发送。重连后立即补发当前完整状态。
已经入队的 QoS 1/2 消息仍按
MQTT 协议重传，因此可能重复；接收端不能把每条状态消息当作新增产量。

## 原生配置与状态接口

配置存储在 NVS `jsp32_config` 中：

| NVS 键 | 类型 | 默认值 |
| --- | --- | --- |
| `mqtt_enabled` | u8 | 0 |
| `mqtt_url` | string | 空，最多 256 字节 |
| `mqtt_username` | string | 空，最多 128 字节 |
| `mqtt_password` | string | 空，最多 128 字节 |

启用项必须填写 URL。认证通过独立字段提供，URL 不接受内嵌用户名和密码。
密码不能通过通用 NVS 读取接口或 MQTT 配置接口取回。

- `GET /api/v1/mqtt/config`：返回保存的配置对象，密码仅返回 `passwordSet`。
- `PUT /api/v1/mqtt/config`：提交配置对象，写入后重启生效。
- `GET /api/v1/mqtt/status`：返回设备 ID、连接状态、入队/拒绝次数及当前报告。

PUT 中省略 `password` 保留已有密码；显式 `"password": ""` 清除。

```json
{
  "enabled": true,
  "url": "mqtt://192.168.1.10:1883",
  "username": "",
  "password": ""
}
```

上报服务不提供 MQTT 订阅、远程命令或寄存器写入接口。
