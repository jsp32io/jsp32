# Modbus TCP Server

固件默认在 TCP 端口 502 启动 Modbus TCP Server。它直接访问原生 RAM 寄存器表，
与 JavaScript、HTTP 和 RTU 从站共享数值；重启后寄存器值归零。服务监听所有已启用的
网络接口，最多同时接入 `CONFIG_MODBUS_TCP_MAX_CLIENTS` 个客户端，空闲连接
30 秒后关闭。支持功能码 01、02、03、04、05、06、0F、10，每次事务最多
`APP_REGISTERS_MAX_TRANSACTION` 项，位操作上限为 256 项。

| Modbus 区域 | PDU 地址 | 应用寄存器 | 权限 |
| --- | --- | --- | --- |
| Discrete inputs | 0 起 | X0 起 | 只读 |
| Coils | 0 起 | Y0 起，随后 M0 起 | 读写 |
| Input registers | 0 起 | D0 起，长度为 `floor(D容量/2)` | 只读 |
| Holding registers | 0 起 | `D[floor(D容量/2)]` 起 | 读写 |

请求不能跨越 Y/M 映射段。非法地址、数量和功能码返回相应 Modbus 异常。
报文使用 MBAP 头，不包含 RTU CRC；Transaction ID 和 Unit ID 在响应中原样返回。

## NVS 配置

`jsp32_config` 命名空间中有两个可选项：`modbus_enabled` 为 `u8`，值 0 或 1，
缺省为 1；`modbus_port` 为 `u16`，范围 1～65535，缺省为 502。通过已有 NVS HTTP API
设置，写入后重启生效：

```sh
curl -X POST http://DEVICE_IP/api/v1/nvs/namespaces/jsp32_config/batch \
  -H 'Content-Type: application/json' \
  -d '{"operations":[{"op":"set","key":"modbus_enabled","type":"u8","value":1},{"op":"set","key":"modbus_port","type":"u16","value":1502}]}'
```

删除某个键后恢复该键的默认值。类型错误或越界值会在启动时记录警告，并对该键
使用默认值。监听失败会记录错误，其余设备服务继续运行。Modbus TCP 本身没有
身份验证；可写区域只应暴露在可信网络中。

默认固件配置将 `CONFIG_LWIP_MAX_SOCKETS` 设为 24，以便与 HTTP 和 MQTT 连接共存。
已有的 `sdkconfig` 不会因修改 `sdkconfig.defaults` 自动更新，需要自行调整后构建。

主机端协议测试：`python3 tests/modbus_tcp_host/run.py`。
