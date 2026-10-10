# JSP32 手册

设备固件提供的使用手册。使用 `cli man <主题>` 查阅，正文直接输出 Markdown。

| 主题 | 手册 | 内容 |
| --- | --- | --- |
| `js` | [JavaScript API](js/index.md) | 运行环境、日志、定时器、事件、外设、寄存器及标准库 |

```sh
cli man          # 本索引：man/index.md
cli man js       # JavaScript 索引：man/js/index.md
cli man js.gpio  # GPIO 手册：man/js/gpio.md
cli man js.pcnt  # PCNT 手册：man/js/pcnt.md
cli man js.adc   # ADC 手册：man/js/adc.md
```

主题中的点号对应目录层级；目录主题读取该目录的 index.md，页面主题省略
`.md` 扩展名。`cli man index` 与 `cli man` 等价。

HTTP 对应 `GET /api/v1/man`、`GET /api/v1/man/js` 和
`GET /api/v1/man/js.gpio`。响应为 `text/markdown; charset=utf-8`，未知主题
返回 404。手册嵌入应用固件，随固件更新。
