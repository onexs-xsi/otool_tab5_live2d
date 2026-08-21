# NOTICE

本组件包含以下第三方代码/文件，均保留原许可证头：

| 文件 | 来源 | 许可证 | 用途 |
| --- | --- | --- | --- |
| `test_apps/parser_and_adapters/third_party/cjson/cJSON.c` | [DaveGamble/cJSON](https://github.com/DaveGamble/cJSON) tag `v1.7.18`（与 managed `espressif/cjson ^1.7.19~2` 同源上游） | MIT | 仅宿主单元测试编译（固件使用 ESP-IDF 组件管理器拉取的 `espressif/cjson`） |

参考实现（仅审计参考，未复制代码，见实施计划 §2.3）：

- `volcengine/onesdk` 审计快照 `657fceccab227bd860c984cdf37aeae3e68c5b4a`
- `espressif/esp-claw` 审计快照 `fb7b248114bb1b12ba0fe8e03d4b59bdbec292c1`

宿主测试工具：TinyCC 0.9.27（仅本机开发环境使用，不随仓库分发）。
