# 开发和部署插件

[JavaScript API 索引](index.md)

内置插件的使用说明见 [io 插件](plugins/io.md) 和 [online 插件](plugins/online.md)。
插件可通过 [telemetry 声明](telemetry.md) 将寄存器数据上报到原生 MQTT 服务。
插件页可自定义上报字段和参数，设置保存在 `/plugins/<名称>/telemetry.json`，
修改后重启生效。文件不存在时使用插件声明；文件存在时完全使用文件配置。

一个插件放在 `/plugins/<名称>/index.js`。名称由小写字母开头，只能包含
小写字母、数字、`_` 和 `-`，最长 32 个字符；名称 `id`、`onCleanup` 和
`config` 不可使用。目录名就是插件 ID，不需要在 JS 中再次声明。

## 编写插件

下面是 `/plugins/heartbeat/index.js`：

```js
export default {
  config: {
    intervalMs: {type: "integer", name: "心跳间隔", default: 1000, min: 100, max: 60000},
    message: {type: "string", default: "alive"},
  },
  setup({id, config, onCleanup}) {
    const timer = setInterval(
      () => console.log(`${id}: ${config.message}`), config.intervalMs);
    onCleanup(() => clearInterval(timer));
  },
};
```

插件必须默认导出含 `setup(context)` 的对象。`setup` 可以是异步函数；
需要使用的 GPIO、I²C、定时器等接口与普通 JS 脚本相同。申请资源后立即
用 `onCleanup(fn)` 登记清理函数；启动中途失败或主动停止插件时，已登记的
函数会按相反顺序执行。

如果插件需要另一个插件提供的服务，声明其目录名：

```js
export default {
  deps: ["logger"],
  setup({id, logger}) {
    logger.info(`${id} started`);
  },
};
```

`deps` 可省略。每个声明的依赖会先启动，其 `setup` 返回值以相同名称传入
当前插件的 `context`。依赖缺失、启动失败或存在循环时，当前插件不会启动。
插件模块的顶层只放定义；外设初始化和监听器注册放在 `setup` 中。

## 声明设置项

在默认导出对象的 `config` 中声明设置项。每项指定 `type`，可选
`name`、`default`、`choices`；`integer` 和 `number` 还可指定 `min`、`max`。
`name` 是用于展示的字符串，省略时展示端可使用设置项名称。
支持的类型是 `string`、`boolean`、`integer`、`number`。没有 `default`
的项必须在设置文件中提供。设置项名称由字母开头，只能包含字母、数字和
`_`。

设备上的 `/plugins/heartbeat/config.json` 只需写覆盖默认值的项：

```json
{"intervalMs": 2000}
```

设置文件不存在时使用默认值；未知项、类型或范围错误会阻止该插件启动。
`setup` 收到的 `config` 是合并后的只读对象。修改文件后重启设备生效。

加载完成后，其他 JS 模块可查询插件的配置声明与当前生效值：

```js
import {plugins} from "plugins";
const {schema, values} = plugins.getConfig("heartbeat");
```

`schema` 是插件声明的可配置项，`values` 是文件覆盖默认值后的结果。
查询不存在的插件返回 `undefined`；声明有效但配置文件校验失败时，
`schema` 仍可读取，`values` 为 `null`。查询结果不可修改。

## 启用和禁用插件

在设备的 `/plugins/meta.json` 中按插件 ID 声明元数据：

```json
{
  "heartbeat": {"disabled": true}
}
```

`disabled` 必须是布尔值。文件不存在、插件未声明或省略 `disabled`
时，默认 `false`，插件正常加载。启用插件时将 `disabled` 改为 `false`
或删除该字段。修改并上传文件后重启设备生效，其他元数据字段允许保留。
元数据文件格式或字段类型错误会停止插件加载，并输出错误日志。

禁用插件不会导入 `index.js` 或执行 `setup`，状态为 `disabled`。
依赖它的插件会被跳过，独立插件继续运行。

```js
import {plugins} from "plugins";
plugins.getMeta("heartbeat"); // {disabled: true}
plugins.list();               // 包含各插件的 disabled 和 status
plugins.stats();              // 包含 disabled 计数
```

`getMeta(id)` 返回元数据副本并补齐 `disabled: false` 默认值；不存在的
插件返回 `undefined`。禁用插件的 `getConfig(id)` 返回
`{schema: null, values: null}`。

准备本地 `meta.json` 后，使用 CLI 上传：

```sh
JSP32_BASE_URL=http://192.168.1.20 ./cli/cli fs put /plugins/meta.json meta.json
JSP32_BASE_URL=http://192.168.1.20 ./cli/cli reboot -wait
```

## 查询未生效的更改

修改配置或启用状态后，可在重启前查询差异：

```js
import {plugins} from "plugins";
plugins.getPendingChanges("heartbeat");
// {
//   config: {intervalMs: {current: 1000, pending: 2000}},
//   meta: {disabled: {current: false, pending: true}}
// }
```

每次查询都会读取当前文件。`config` 比较运行中插件的生效值与当前
`config.json` 按已加载声明合并默认值后的结果；`meta.disabled` 比较
启动时读取的值与当前 `/plugins/meta.json`，省略时使用 `false`。
未运行的插件仅比较启用状态。

结果只包含有差异的项和分组；无差异返回 `{}`，不存在的插件返回
`undefined`。删除配置项或配置文件会按默认值比较，缺少必填项、文件
读取失败、JSON 格式或校验错误时抛出异常。调整 JSON 格式或恢复原值
不会产生差异。查询不会应用更改；它使用已加载的配置声明，不检测
插件代码中的声明变化。

## 部署到设备

在仓库根目录准备本地文件 `heartbeat/index.js` 和
`heartbeat/config.json`，然后用 CLI 上传。首次部署时创建目录；将示例
IP 换成设备地址：

```sh
(cd cli && go build -o cli .)
JSP32_BASE_URL=http://192.168.1.20 ./cli/cli fs mkdir /plugins
JSP32_BASE_URL=http://192.168.1.20 ./cli/cli fs mkdir /plugins/heartbeat
JSP32_BASE_URL=http://192.168.1.20 ./cli/cli fs put /plugins/heartbeat/config.json heartbeat/config.json
JSP32_BASE_URL=http://192.168.1.20 ./cli/cli fs put /plugins/heartbeat/index.js heartbeat/index.js
JSP32_BASE_URL=http://192.168.1.20 ./cli/cli reboot -wait
```

已有目录不用再次创建。所有设置项都有默认值时，可以不上传 `config.json`。
插件包含多个 JS 文件时，先上传被导入的文件和设置文件，最后上传
`index.js`，全部传完再重启。插件文件在 JS 的 `fs` API 中只读，部署
使用上述 CLI 文件接口。更新插件后也要重启，新的模块代码才会生效。
