# Application HTTP server

共享 HTTP 服务提供系统信息 `/api/v1/system/info`、手册查询及其他组件注册的路由。
独立 `app_ui` 组件注册内置 UI 根路径和静态资源。`network_manager` 注册网络 API
及系统信息 provider；Wi-Fi 查询和发射功率设置由网络模块执行，HTTP 服务负责信息聚合。

系统信息的 `network` 保持原有结构，仅包含 `station`、`softAp`、
`wifiMaxTxPowerQdbm` 和 `wifiMaxTxPowerDbm`。模式、扫描、操作结果等状态
通过 `/api/v1/network/status` 提供。

系统信息中的 `network.station.ssid` 是当前连接的 Wi-Fi 名称，
`network.station.rssi` 是信号强度，单位 dBm。每次请求实时查询驱动；
未连接或查询失败时两个字段均为 null。`cli info` 会直接显示这些字段。

`network.wifiMaxTxPowerQdbm` 和 `network.wifiMaxTxPowerDbm` 是每次从 Wi-Fi
驱动读取的当前最大发射功率，分别以四分之一 dBm 和 dBm 表示；驱动尚未启动或
读取失败时为 null。

## Wi-Fi 发射功率

```http
POST /api/v1/system/tx-power
Content-Type: application/json

{"maxTxPowerQdbm":52}
```

`maxTxPowerQdbm` 必须是 8 到 84 的整数，即 2 到 21 dBm。ESP-IDF 会将请求值
映射到硬件支持的档位；成功时返回 `200 OK`，例如
`{"maxTxPowerQdbm":52,"maxTxPowerDbm":13}`，数值来自设置后的驱动读取。
请求无效返回 400，Wi-Fi 未启动或不可用返回 503，NVS 保存失败返回 500。
修改立即生效并保存到 NVS；重启后网络管理器会重新应用保存值。首次设置前
使用固件配置的默认值。

## 手册查询

```http
GET /api/v1/man/js.gpio
```

成功返回 `200 OK`，`Content-Type: text/markdown; charset=utf-8`。
正文是手册文件的原始 Markdown 字节，不包 JSON，不转换 HTML，也不重写链接。
未知主题返回 `404 Not Found`。

| URL | 对应文件 |
| --- | --- |
| `/api/v1/man`、`/api/v1/man/`、`/api/v1/man/index` | `man/index.md` |
| `/api/v1/man/js`、`/api/v1/man/js.index` | `man/js/index.md` |
| `/api/v1/man/js.gpio` | `man/js/gpio.md` |
| `/api/v1/man/js.adc` | `man/js/adc.md` |

目录主题读取该目录的 index.md。主题名使用点号分隔目录与文件名，省略 `.md`。
查询参数不参与主题查找；不解码主题中的百分号转义，不接受斜线形式的文件路径。
返回 `Cache-Control: no-cache`，避免固件更新后继续使用未验证的缓存。

手册通过 `tools/embed_man.py` 在构建时从 `man/**/*.md` 自动生成并嵌入固件。
新增、删除和修改页面都会触发相应构建更新，不依赖 LittleFS 或 JavaScript
Runtime 成功启动。目录及文件名（去除扩展名）需匹配 `[a-z0-9][a-z0-9_-]*`。
更新手册需要重新编译并部署应用固件。

```sh
curl http://jsp32.local/api/v1/man/js.gpio
JSP32_BASE_URL=http://jsp32.local ./cli man js
JSP32_BASE_URL=http://jsp32.local ./cli man js.gpio
```

主机验证：`python3 tests/man_host/run.py` 编译实际 HTTP handler，通过 HTTP
桩验证所有页面与源 Markdown 字节一致、目录索引、查询参数及未知/非法主题。
CLI 测试在 `cli` 目录运行 `go test ./...`。
