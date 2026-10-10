# QuickJS ESP-IDF component

源码固定为 Fabrice Bellard QuickJS **2025-09-13**（修订发布包 `-2`），以 MIT 许可证分发，许可证保留在 `upstream/LICENSE`。

- 下载：https://bellard.org/quickjs/quickjs-2025-09-13-2.tar.xz
- SHA-256：`996c6b5018fc955ad4d06426d0e9cb713685a00c825aa5c0418bd53f7df8b0b4`
- API 文档：https://bellard.org/quickjs/quickjs.html

仅保留引擎构建所需的 C 源文件、头文件、版本与许可证；不编译桌面端 `qjs`、`qjsc`、`quickjs-libc`。

`upstream/quickjs.c` 中的平台适配受 `ESP_PLATFORM` 条件保护：

1. 禁用依赖线程及 64 位原子操作的 `Atomics.*`，保留栈检查。
2. 默认运行时分配器通过 `heap_caps_malloc/realloc` 使用 PSRAM；ESP-IDF 的 `free` 可以释放这些内存。
3. 使用 `heap_caps_get_allocated_size` 获取分配大小，保留 QuickJS 内存限额统计。
4. 通过标准 `gmtime_r/localtime_r` 计算时区偏移，兼容没有 `tm_gmtoff` 的 C 库。

另将四处传给 `int32_t *` 参数的局部变量改为 `int32_t`，兼容 Xtensa 上 `int32_t` 为 `long` 的类型定义。
TypedArray 构造函数按 `argc` 读取可选的 offset/length 参数，避免原生 `JS_NewTypedArray(ctx, 1, ...)` 越界读取未传入的参数；这曾导致寄存器批量读取返回空数组或触发 panic。

在创建运行时前必须初始化 PSRAM。一个运行时应始终在同一 FreeRTOS 任务中使用；此组件不提供跨任务同步。构建时使用上游要求的 `-fno-strict-aliasing`。
