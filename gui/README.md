# JSP32 桌面控制台

基于 SDL3、Dear ImGui 和 OpenGL 的 Windows 10+ / Linux 桌面上位机。主界面采用浅色紧凑布局，保留设备切换入口；主导航包含“概览”“寄存器”和“插件”。

## Linux 构建与运行

需要 C++20、CMake 3.21+，以及 SDL3、OpenGL、libcurl、jsoncpp、libarchive、Avahi client、fontconfig 的开发包和运行中的 Avahi 服务。Dear ImGui 1.92.9b 源码已放在 `vendor/imgui`。

```sh
cmake -S gui -B build/gui
cmake --build build/gui -j
ctest --test-dir build/gui --output-on-failure
./build/gui/jsp32-gui
```

## Windows 构建与打包

在 **MSYS2 UCRT64** 终端中安装依赖并构建：

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-curl \
  mingw-w64-ucrt-x86_64-jsoncpp mingw-w64-ucrt-x86_64-libarchive
cmake -S gui -B build/gui-windows -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/gui-windows --parallel
ctest --test-dir build/gui-windows --output-on-failure
cpack --config build/gui-windows/CPackConfig.cmake -G ZIP -B build
```

`build/jsp32-gui-windows-x64.zip` 包含 `jsp32-gui.exe` 和运行所需 DLL，可直接解压运行。Windows 设备发现使用系统 DNS-SD，不依赖 Avahi；中文字体从系统字体目录加载。仓库的 [Windows GUI 工作流](../.github/workflows/gui-windows.yml)执行构建、测试并上传 ZIP 工件。

首次启动自动浏览局域网 `_jsp32._tcp` 服务；发现页的“手动添加”按钮可打开弹窗，输入 IP 地址或 URL 连接。连接时会验证 `/api/v1/system/info` 中的固件标识并以设备站点 MAC 去重。发现页定期验证设备，显示固件版本；在线显示“连接”，离线的已保存设备显示最近记录的版本以及“进入”和“删除”。删除会清除本地保存的设备及其监控项。选中的设备保存在本机 SDL 偏好目录的 `settings.json` 中。再次打开会直接进入上次选中的设备。

概览页展示 `/api/v1/system/info` 返回的系统信息，进入页面后自动读取并定期刷新，也可手动刷新。页面以 dBm 显示驱动当前生效的 Wi-Fi 最大发射功率，并从 7–20 dBm 的硬件支持档位中选择，通过 `/api/v1/system/tx-power` 修改；修改后重新读取系统信息。设置保存到设备 NVS，重启后继续生效。

寄存器页按 X/Y/M/D 区域分页浏览地址和当前值，每页 32 个寄存器。数值通过 SSE 实时更新，只按 range 订阅当前页；切换区域或翻页会更新订阅，离开寄存器页会关闭连接。进入页面或翻页时先读取 HTTP 快照，订阅连接成功、断线重连和收到 `dropped` 时再读取快照补齐数值，并通过修订号避免旧快照覆盖新事件。底部显示订阅与同步状态。设备重启后寄存器归零，这是固件的 RAM 寄存器行为。

插件页读取设备 `/plugins` 目录，支持上传和下载 ZIP、在弹窗中按插件定义编辑配置表单、启用或禁用插件。ZIP 仅接受一个 `<插件 ID>/` 根目录，必须包含 `index.js`。上传时保留设备上已有的 `config.json`，如需改变配置，可点击“配置”打开表单并保存。更改在设备重启后生效。

## 模块

- `src/main.cpp`：SDL3、OpenGL、ImGui 初始化与主循环
- `src/app.*`、`src/ui_*.cpp`：页面状态、异步任务与界面
- `src/discovery_*.cpp`：Linux Avahi / Windows DNS-SD 浏览
- `src/api.*`：固件 HTTP API 客户端
- `src/register_stream.*`：SSE 解码、订阅连接、重连与修订号合并
- `src/model.*`：设备设置、寄存器地址和数据编解码
- `src/font_*.cpp`：平台字体加载
- `src/plugin_config.*`：插件配置表单数据、默认值与校验
- `src/plugin_zip.*`：ZIP 校验、打包和文件系统 API 上传下载
