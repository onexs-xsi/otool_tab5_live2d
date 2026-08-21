# test_fixtures

脱敏协议 fixture。规则（见实施计划 §12）：

1. 手工最小 fixture：覆盖每个边界与错误（`minimal/`）。
2. 官方 SDK 录制 fixture：主机脚本调用官方 SDK 后保存，删除 Key、用户敏感文本与完整 header（`recorded/`，尚未生成）。

## minimal / openai_responses

`ok_full.jsonl`：一次完整 Responses 流（created → 3× output_text.delta → done → completed+usage），与 `test_apps/local_sse_server.py` 的 `/ok` 端点同源：

```jsonl
{"type":"response.created","response":{"id":"resp_local_001","model":"local-model","status":"in_progress"}}
{"type":"response.output_text.delta","item_id":"i1","output_index":0,"response_id":"resp_local_001","delta":"你好，"}
{"type":"response.output_text.delta","item_id":"i1","output_index":0,"response_id":"resp_local_001","delta":"world 🌍"}
{"type":"response.output_text.delta","item_id":"i1","output_index":0,"response_id":"resp_local_001","delta":" (3rd chunk)"}
{"type":"response.output_text.done","item_id":"i1","output_index":0,"response_id":"resp_local_001","text":"你好，world 🌍 (3rd chunk)"}
{"type":"response.completed","response":{"id":"resp_local_001","status":"completed","usage":{"input_tokens":9,"output_tokens":21,"total_tokens":30}}}
```

## minimal / ark_responses

与 OpenAI 同构（方舟 `/api/v3/responses` 为 OpenAI 兼容），差异点在错误体（`error.request_id`）：

```json
{"error":{"code":"InvalidParameter","message":"bad model","request_id":"20260822120000A1B2C3"}}
```

## minimal / openai_chat

`ok_stream.jsonl`：Chat 流（role chunk → 2× content delta → finish_reason + usage → [DONE]）：

```jsonl
{"id":"chatcmpl-local","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"role":"assistant"},"finish_reason":null}]}
{"id":"chatcmpl-local","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":"Hi"},"finish_reason":null}]}
{"id":"chatcmpl-local","object":"chat.completion.chunk","choices":[{"index":0,"delta":{"content":" from chat"},"finish_reason":null}]}
{"id":"chatcmpl-local","object":"chat.completion.chunk","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":4,"completion_tokens":6,"total_tokens":10}}
[DONE]
```

## minimal / ark_chat

`error_401.json`：方舟 Chat 401 错误体：

```json
{"error":{"code":"AuthenticationError","message":"invalid api key","request_id":"req-ark-401"}}
```
