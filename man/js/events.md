# Events API

[返回索引](index.md)

全局 `events` 用于在 EventLoop 中投递字符串消息。当前事件名只支持
`"message"`，其他事件名抛出 `RangeError`。

## 接口

| 方法 | 返回值 | 行为 |
| --- | --- | --- |
| `events.on("message", callback)` | `undefined` | 添加监听器，callback 必须是函数 |
| `events.off("message", callback?)` | boolean | 移除匹配监听器；实际移除时为 true |
| `events.post(value)` | boolean | 将 value 转为字符串并入队；成功为 true |
| `events.stats()` | `{queued, dropped}` | 当前运行时队列长度与累计丢弃数 |

```js
function receive(message) {
  console.log("received:", message);
}
events.on("message", receive);
const accepted = events.post("ready");
console.log("queued:", accepted);
// 不再接收时执行 events.off("message", receive);
```

post 只入队，不会同步调用监听器。返回 true 表示成功入队，不表示已被监听器
处理；队列满或停止接收时返回 false。字符串转换失败或分配失败会抛出异常。
发送对象时请显式 `JSON.stringify`，监听器收到的是字符串。

## 订阅与队列

重复注册产生独立监听项；当前实现按后注册先调用的顺序执行。
off 指定 callback 时删除该函数的所有匹配项，省略或传入 undefined 时
删除全部 message 监听器。取消监听需要保留原函数引用。

消息分发时使用监听器快照；在一个回调内增删监听器不会改变当前消息已取得的
回调列表。post 后、实际分发前取消监听，会使该监听器收不到这条消息。

stats 统计运行时共享事件队列，包含其他原生事件，并非仅 message 消息。
容量由 `CONFIG_JS_EVENT_QUEUE_LENGTH` 控制，默认 32。不要把 post 当作保证
送达的持久消息系统；需要时检查布尔返回值和 dropped 计数。
