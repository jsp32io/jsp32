# JSP32 CLI for npm

English | [简体中文](README_zh.md)

Install the JSP32 command-line tools with Node.js 18 or later:

```sh
npm install -g @jsp32/jsp32
jsp32 --help
```

The public package `jsp32` forwards to the same version of `@jsp32/jsp32`:

```sh
npm install -g jsp32
npx jsp32 --help
# Or run the scoped package directly:
npx @jsp32/jsp32 --help
```

Both packages expose the `jsp32` command; choose either for global installation.
Arguments, standard input/output, environment variables and exit codes are
passed to the native CLI. See the [CLI guide](../README.md) for commands.

| Platform | Binary package |
| --- | --- |
| Windows amd64 | `@jsp32/win32-x64` |
| Windows arm64 | `@jsp32/win32-arm64` |
| Linux amd64 | `@jsp32/linux-x64` |
| Linux arm64 | `@jsp32/linux-arm64` |
| macOS arm64 | `@jsp32/darwin-arm64` |

npm selects the matching binary through optional dependencies. Keep optional
dependencies enabled; installation needs no Go compiler or postinstall download.
Other platforms, including Intel macOS, are unsupported.

## Build and publish

From `cli/npm`, with Go 1.25 or later and Node.js 18 or later:

```sh
npm test
npm run pack:all
```

This cross-compiles all five binaries and creates seven tarballs in `dist/`.
No npm dependency installation is needed to build. Binaries use `CGO_ENABLED=0`;
on macOS, automatic USB VID/PID detection is unavailable, so use explicit
`flash -reset usb-jtag` for native USB Serial/JTAG.
The build copies the root package version into every package and pins all
internal dependencies to that exact version. For a new release, set the version
with `npm version 0.1.1 --no-git-tag-version` before building.

Publish with an npm account authorized for the `@jsp32` scope and `jsp32`:

```sh
# Run tests, build, pack and preview all seven publications:
npm run publish:all -- --dry-run
# Run tests, build, pack and publish all seven packages:
npm run publish:all
```

The script publishes the five binary packages first, then `@jsp32/jsp32`, then
`jsp32`, and stops immediately on failure. Every publication explicitly uses
`--access public --tag latest`, so both entry points' `latest` tags point to the
released version. `publishConfig.tag` is also set to `latest` in every package.
Each release requires a new version; npm cannot overwrite a published version.
`pack:all` only builds and packs, without publishing.
Package metadata uses `UNLICENSED` because this repository does not currently
declare a project license.
