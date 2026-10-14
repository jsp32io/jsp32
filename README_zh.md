# JSP32

[English](README.md) | 简体中文

JSP32 是面向 ESP32-S3 的 JavaScript 可编程设备固件。你可以用脚本读取传感器、
控制数字 I/O、处理串口与网络通信，再通过寄存器、Modbus 和 MQTT 与上位系统连接。
设备自带网页管理界面，也提供命令行客户端和 Windows / Linux 桌面控制台。

默认硬件配置为 ESP32-S3 N16R8：16 MB Flash、8 MB Octal PSRAM。

## 项目特色

- **用 JavaScript 编写设备逻辑**：QuickJS 支持 ES 模块、Promise、定时器和事件监听。
  脚本存放在设备文件系统中，更新业务逻辑无需重新编译固件。
  异步读取统一使用 `{timeoutMs, signal}` 选项：0 表示无限等待，等待超时以
  `code: "TIMEOUT"` 拒绝 Promise；AbortController 可取消单次读取而无需关闭句柄。
- **插件化部署与配置**：插件按依赖顺序启动，可以声明设置项、登记资源清理函数，
  并在网页或桌面控制台中查看状态、修改配置。
- **丰富的硬件接口**：GPIO 输入输出与去抖、脉冲和正交编码器计数、ADC 采样、
  异步 UART 读写与分帧、I²C 设备访问。
- **统一的寄存器数据**：PLC 风格的 X/Y/M/D 寄存器由脚本、HTTP API 和 Modbus
  共享，支持原子读写及变化订阅，便于连接设备逻辑与上位机。
- **网络通信与遥测**：支持 TCP 客户端和监听、TLS 客户端、Modbus TCP 服务端、
  JavaScript Modbus RTU 从站，以及插件数据的 MQTT 定时或变化上报。
- **设备管理开箱可用**：网页提供系统概览、Wi-Fi 配置、寄存器实时浏览、插件和
  MQTT 设置；局域网设备发现、日志、文件管理、远程脚本执行和 OTA 均有配套接口。
- **原生服务与脚本分工明确**：Wi-Fi 管理和 HTTP 服务独立于 JavaScript 运行。
  EventLoop 统一调度脚本回调、Promise 和外设事件，使用执行预算与资源上限控制负载。

## 使用设备

未保存网络模式的设备启动为 `JSP32-XXXX` 热点。连接后，在浏览器打开
`http://192.168.4.1/`。网络页可以选择仅热点（AP）、连接外部 Wi-Fi（STA），
或同时运行两者（APSTA）；网络设置保存在设备中，重启后恢复。

| 使用入口 | 适合的操作 |
| --- | --- |
| 内置网页 | 查看设备信息、配置网络、浏览实时寄存器、配置插件和 MQTT |
| [桌面控制台](gui/README.md) | 发现局域网设备、监控寄存器、上传下载插件 ZIP、管理插件配置 |
| [命令行客户端](cli/README.md) | 脚本求值、实时日志、文件和寄存器操作、NVS 管理、串口烧录和 OTA 固件更新 |
| [HTTP API](components/app_http_server/README.md) | 将设备管理能力接入其他工具或系统 |

网页入口为 `/`、`/network`、`/registers`、`/plugins` 和 `/mqtt`。
设备通过 mDNS 广播 `_jsp32._tcp.local` 服务，桌面控制台可以自动发现同一局域网的设备，
也支持手动输入地址。管理接口没有鉴权，应在可信网络中使用。

## 编写设备逻辑

设备启动时执行 `/main.js`，由插件加载器启动 `/plugins/<名称>/index.js`。
插件可以使用全局硬件接口，也可以导入 `/stdlib` 中的 JavaScript 库。

例如，一个每秒输出心跳的插件：

```js
export default {
  setup({onCleanup}) {
    const timer = setInterval(() => console.log("alive"), 1000);
    onCleanup(() => clearInterval(timer));
  },
};
```

将它保存到设备的 `/plugins/heartbeat/index.js`，重启后运行。
插件文件通过 CLI 的文件接口或桌面控制台部署；网页用于查看状态和修改设置。
完整步骤及配置声明见 [插件开发与部署](man/js/plugins.md)。

| 能力 | 使用手册 |
| --- | --- |
| 脚本运行、模块、日志、定时器和事件 | [JavaScript API 索引](man/js/index.md) |
| GPIO、PCNT、ADC、UART、I²C | [外设接口](man/js/index.md#外设与寄存器) |
| X/Y/M/D 寄存器 | [寄存器](man/js/registers.md) |
| TCP 与 TLS | [网络通信](man/js/net.md) |
| MQTT 遥测字段及发送规则 | [遥测数据](man/js/telemetry.md) |
| Modbus RTU / TCP | [RTU 从站](man/js/modbus.md) · [TCP 服务端](components/modbus_tcp/README.md) |
| 数字输入与编码器计长 | [io 插件](man/js/plugins/io.md) |
| TCP 转发与远程访问 | [online 插件](man/js/plugins/online.md) |
| 文件管理 | [文件系统](man/js/fs.md) |

脚本环境不提供浏览器的 `fetch`、桌面端 `std/os` 模块或 npm 包解析。
寄存器数据保存在 RAM 中，重启后归零；需要持久化的数据应写入文件或通过 NVS 管理接口保存。

## 项目结构

```text
main/                  固件启动入口，组装原生服务与 JavaScript 模块
components/            原生设备服务、JavaScript 绑定和随仓库提供的第三方依赖
  js_runtime/          QuickJS EventLoop、定时器、监听器与异步请求管理
  js_gpio/ ...         GPIO、PCNT、ADC、UART、I²C、网络等脚本接口
  app_ui/              嵌入固件的网页界面
  network_manager/     Wi-Fi 模式、配置保存与连接恢复
  app_registers/       原生寄存器数据与事务
  app_mqtt/            MQTT 连接与遥测上报
rootfs/                LittleFS 镜像内容
  main.js              JavaScript 入口
  plugins/             设备插件
  stdlib/              JavaScript 标准库与驱动库
  examples/            脚本示例
man/                   随固件提供的设备使用手册
cli/                   Go 命令行客户端
gui/                   C++ 桌面控制台（SDL3 / Dear ImGui）
tests/                 主机测试与集成测试
tools/                 烧录、Flash 擦除和手册嵌入工具
sdkconfig.defaults     默认硬件与功能配置
partitions.csv         16 MB Flash 分区布局
```

## 构建与部署

固件使用 ESP-IDF v6.1.0 和 ESP32-S3 工具链。在项目根目录加载环境后构建：

```sh
. /path/to/esp-idf/export.sh
idf.py build
```

应用固件为 `build/jsp32.bin`，文件系统镜像为 `build/storage.bin`。
QuickJS、cJSON、mDNS、MQTT 等依赖随仓库存放在 `components/`，普通固件构建无需从组件注册表下载依赖。
默认配置使用 QIO 40 MHz Flash、Octal PSRAM 80 MHz，以及原生 USB Serial/JTAG 控制台。
已有 `sdkconfig` 的设置通过 `idf.py menuconfig` 调整，`sdkconfig.defaults` 用于生成初始配置。

对于使用 S25FL128S1 Flash 的板子，串口更新应用使用仓库提供的烧录脚本：

```sh
python tools/s25fl_flash_app.py --port /dev/ttyACM0 --address 0x20000 build/jsp32.bin
idf.py -p /dev/ttyACM0 monitor
```

将串口替换为实际设备路径。该 Flash 的主阵列使用 64 KiB 擦除块，脚本会补齐应用镜像后写入，
避免普通烧录工具的尾块擦除问题。脚本仅更新应用；首次部署或更换分区布局时，需要通过串口
写入匹配的 bootloader、分区表、应用和文件系统镜像，并先备份设备数据。
Flash 几何约束、频率和板级注意事项见 [S25FL128S1 组件说明](components/s25fl128s/README.md)。

默认分区包含两个 3 MiB OTA 应用槽和 9.875 MiB LittleFS 存储空间。
应用槽起始地址为 `0x20000` 和 `0x320000`，文件系统起始地址为 `0x620000`。
普通 OTA 更新保留文件系统内容。新固件首次启动后须通过
`POST /api/v1/ota/confirm` 确认；确认前再次复位会回滚到上一有效镜像。
操作说明见 [CLI 固件更新](cli/README.md)和 [OTA API](components/ota_http/README.md)。

8 MB Flash 使用 `partitions_8mb.csv`，需同时调整 Flash 容量和分区表配置，
对应 LittleFS 空间为 1.875 MiB。板级接线、可用引脚及 Flash 参数应与实际硬件匹配。

## 文档导航

- [设备手册](man/index.md)：面向使用者的 API、插件和标准库说明，也可通过 CLI 的 `man` 命令在设备上查阅。
- [内置网页](components/app_ui/README.md)、[桌面控制台](gui/README.md)、[命令行客户端](cli/README.md)：管理工具的使用和构建说明。
- [Runtime 组件](components/js_runtime/README.md)：原生模块接入、EventLoop 调度与资源生命周期。
- 各组件目录中的 README：对应服务的配置、HTTP 接口或原生实现说明。
