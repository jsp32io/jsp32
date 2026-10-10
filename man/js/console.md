# Console API

[返回索引](index.md)

## `console.log(...values)`

将参数逐一转换为字符串，以空格分隔并输出换行，返回 `undefined`。
日志同时送往串口及供远程控制台订阅的历史缓冲。

```js
console.log("ADC", adc.readVoltage(3), "mV");
console.log(JSON.stringify({ready: true, count: 2}));
```

对象使用普通字符串转换，不自动进行结构化展开；查看对象内容可显式调用
`JSON.stringify`。不提供 `%s`、`%d` 等格式占位符处理。
参数的字符串转换抛出异常时，异常会传播给调用者。

## 缓冲与限制

远程日志每条最多保留 1023 字节，超长内容截断；其中的回车和换行替换为空格。
串口直接输出参数文本，不受该历史行长度限制。
历史条数由 `CONFIG_JS_RUNTIME_CONSOLE_QUEUE_LENGTH` 控制，默认 16 条。
慢速远程消费者可能丢日志。

当前只提供 `console.log`，未提供 `console.warn`、`console.error`、
`console.table` 等方法。远程订阅方式见 [JS HTTP](../../components/js_http/README.md)。
