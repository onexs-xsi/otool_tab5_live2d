# test_fixtures

这里保存脱敏后的协议最小样本。当前里程碑只维护可验证的方舟
Responses/Chat 样本；OpenAI provider 仍保留在组件中，但录制和真实服务验收暂缓，
不会用方舟样本冒充 OpenAI 实测证据。

- `minimal/ark_responses/tool_call.sse`：Responses 工具参数分片和完成事件。
- `minimal/ark_responses/error_401.json`：方舟错误体及 `request_id`。
- `minimal/ark_chat/tool_call.sse`：Chat `tool_calls` 参数分片和 `[DONE]`。
- `minimal/ark_chat/error_401.json`：方舟 Chat 鉴权错误体。

录制真实流时必须删除 API Key、Authorization、Cookie、用户敏感文本和完整
header，只保留协议判断所需字段。主机回归测试中的内嵌样本与这些 fixture
保持同一事件结构。
