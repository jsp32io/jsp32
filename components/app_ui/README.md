# JSP32 内置 UI

独立的固件内置设备界面，入口 `/`，静态资源位于 `/assets/`。页面在 AP、
STA、APSTA 下常驻。概览和网络页不依赖 QuickJS；插件页需要 JavaScript
运行时和文件系统 HTTP API。

概览页直接展示 `/api/v1/system/info` 的所有内容，嵌套字段可展开。寄存器页
通过 `/api/v1/registers` 按 X/Y/M/D 区域分页查看 RAM 寄存器，每页 32 项，
支持地址跳转和手动刷新。当前页使用 SSE 接收变更；进入页面、重连或事件丢失
时重新读取快照。页面只读，不提供寄存器写入。

网络页
通过 `/api/v1/network/...` 提供模式、AP/STA 配置和扫描。扫描按钮和结果放在 STA 连接区域，
仅 STA 和 APSTA 时显示；仅 AP 底层使用 APSTA 以支持扫描，但不连接外部 Wi-Fi。组件只持有界面和
静态资源；驱动、配置存储、状态机及网络 API 由 `network_manager` 负责。

插件页通过 `/api/v1/quickjs/eval` 读取 `plugins.list()`、`plugins.getConfig()`，
列出已安装插件、运行状态和配置声明。设置表单按声明生成，未加载配置声明的
插件提供 JSON 编辑框。配置写入 `/plugins/<id>/config.json`，启用状态写入
`/plugins/meta.json`；两种保存均使用文件 ETag 防止覆盖其他客户端的更新，
重启设备后生效。
插件页的“重启设备”按钮调用 `POST /api/v1/ota/reboot`，确认后等待设备
重新上线并刷新页面。

有 telemetry 声明的插件提供独立的“遥测数据”表单，支持字段编辑、添加和删除、
发送模式、频率、topic、QoS 与 retain。表格中的字段都会上报，删除后不会自动补回。
字段 `name` 用作显示名，可单独编辑；上报 JSON 使用字段键。
字段“变化触发”对应 `trigger`，关闭后仍随完整状态发送，但不主动触发变化上报。
遥测字段表的“实时值”列每秒通过寄存器 HTTP API 读取一次，按当前编辑的
类型、字序、倍率和偏移解码。按设备事务上限分批并保持两字字段在同一事务；
离开页面或进入后台暂停轮询，过期响应不会更新新页面，编辑值不会被刷新覆盖。
插件设置表单在桌面端按两列排列，720px 以下为单列。配置键的小字与显示名称
在同一行，复选框位于标签左侧。
设置写入 `/plugins/<id>/telemetry.json`，使用 ETag 防止并发覆盖；保存前通过
原生 telemetry 解析器校验，不修改当前上报定义。文件存在时完全使用文件配置；
恢复插件默认设置会删除文件，重启生效。

插件操作失败时显示操作名称、插件 ID、错误类型、实际 JS 表达式和服务端
堆栈；HTTP 请求失败时显示接口路径，文件读取失败时显示文件路径，便于定位
缺少接口、运行库版本不一致、配置文件格式和请求失败等问题。

页面采用无外部依赖的 HTML/CSS/JavaScript，使用 CMake `EMBED_TXTFILES`
嵌入固件。修改资源后重新构建固件即可更新页面。浏览器访问 `/`（概览）、
`/network`（网络）、`/registers`（寄存器）、`/plugins`（插件）或 `/mqtt`（MQTT）。导航通过 History API 切换，
支持直接访问、刷新、前进和后退，切换页面时保留表单内容。桌面端使用常驻
侧栏，内容占满剩余宽度；720px 及以下屏幕使用可收起的侧栏，内容占满屏幕。

`app_ui` 注册页面 fallback：未匹配的页面 GET 请求返回 `index.html`，
API、静态资源目录、文件请求及非 GET 请求保留 404。

MQTT 页通过 `/api/v1/mqtt/config` 配置一个 broker，密码留空保留已有
密码，并提供显式清除选项。连接状态和插件上报定义通过 `/api/v1/mqtt/status`
读取。保存后可点击“重启设备”应用配置。

```sh
node --check components/app_ui/assets/app.js
python tests/app_ui_host/run.py
node --test tests/app_ui_host/navigation.test.cjs
node --test tests/app_ui_host/registers.test.cjs
node --test tests/app_ui_host/plugins.test.cjs
```
