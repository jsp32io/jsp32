# JSP32 CLI npm 包

[English](README.md) | 简体中文

使用 Node.js 18 或更新版本安装 JSP32 命令行工具：

```sh
npm install -g @jsp32/jsp32
jsp32 --help
```

公共包 `jsp32` 转发到相同版本的 `@jsp32/jsp32`：

```sh
npm install -g jsp32
npx jsp32 --help
# 也可以直接运行作用域包：
npx @jsp32/jsp32 --help
```

两个包均提供 `jsp32` 命令，全局安装时任选一个。
参数、标准输入输出、环境变量和退出码均传递给原生命令行工具。
命令用法见 [CLI 指南](../README.md)。

| 平台 | 二进制包 |
| --- | --- |
| Windows amd64 | `@jsp32/win32-x64` |
| Windows arm64 | `@jsp32/win32-arm64` |
| Linux amd64 | `@jsp32/linux-x64` |
| Linux arm64 | `@jsp32/linux-arm64` |
| macOS arm64 | `@jsp32/darwin-arm64` |

npm 通过可选依赖选择对应平台的二进制包，请保持可选依赖启用。
安装无需 Go 编译器，也不需要安装后额外下载。
不支持其他平台，包括 Intel macOS。

## 构建与发布

在 `cli/npm` 下使用 Go 1.25 或更新版本及 Node.js 18 或更新版本：

```sh
npm test
npm run pack:all
```

脚本交叉编译五个平台的二进制，在 `dist/` 生成七个 tarball。
构建无需安装 npm 依赖。二进制使用 `CGO_ENABLED=0`；macOS 下无法自动检测
USB VID/PID，使用原生 USB Serial/JTAG 时请显式指定 `flash -reset usb-jtag`。
构建会将根包版本同步到所有子包，并将内部依赖锁定到相同版本。
发布新版本前先执行 `npm version 0.1.1 --no-git-tag-version` 设置版本号。

使用有权发布 `@jsp32` 作用域和 `jsp32` 的 npm 账号：

```sh
# 测试、构建、打包，并预演七个包的发布：
npm run publish:all -- --dry-run
# 测试、构建、打包，并发布七个包：
npm run publish:all
```

脚本先发布五个二进制包，再发布 `@jsp32/jsp32`，最后发布 `jsp32`，
任何一步失败都会立即停止。每个包都显式使用 `--access public --tag latest`，
因此两个入口包的 `latest` 标签都会指向本次发布版本。
每个包的 `publishConfig.tag` 也设为 `latest`。
每次发布需使用新版本号，npm 不允许覆盖已发布版本。
`pack:all` 仅构建和打包，不会发布。
仓库目前未声明项目许可证，因此包元数据使用 `UNLICENSED`。
