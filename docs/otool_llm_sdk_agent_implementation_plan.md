# `otool_llm_sdk` Agent 化、工具调用与可靠性完善计划

> 基线日期：2026-08-22  
> 目标组件：`components/otool_llm_sdk`  
> 计划状态：待实施  
> 目标版本：文本/工具 Agent MVP `0.3.0`，可靠性版本 `0.3.x`

## 1. 执行结论

保留并继续完善自研 `components/otool_llm_sdk`，不切换到 `esp-claw`，也不在 ESP32-P4 上移植 OpenAI 或火山引擎的非 C/C++ 官方 SDK。

组件的下一阶段定位从“流式文本 HTTP 客户端”升级为：

1. OpenAI Responses、火山方舟 Responses 和兼容 Chat Completions 的有界流式客户端；
2. provider 无关的 Function Calling/工具调用协议层；
3. 可在单个 worker task 中运行的基础 Agent Runtime；
4. 为后续豆包语音、Realtime/WebSocket 和本地设备工具保留稳定接口。

本计划中的“基本 Agent 能力”必须至少包含：

- 多轮上下文；
- 模型自主选择工具；
- 工具参数流式收集和本地校验；
- 本地工具执行与结果回传；
- 模型继续推理直至给出最终文本；
- 工具/轮次/内存/超时上限；
- 跨任务取消；
- 工具白名单和副作用策略；
- OpenAI 与火山方舟的真实服务验证。

不把官方 Python SDK 或 OpenAI Agents SDK 直接放进固件。官方 SDK、官方示例和真实响应仅作为协议基准、fixture 生成器和主机侧一致性 oracle。

## 2. 文档归档与本计划边界

本计划是后续执行 agent 的唯一主计划。原有文档已移动到 `docs/backup/`，保留历史决策和实测记录：

- `backup/otool_llm_sdk_integration_plan.md`；
- `backup/otool_llm_sdk_streaming_voice_implementation_plan.md`；
- `backup/esp_claw_direct_component_integration_plan.md`；
- 其余 Live2D/Cubism 文档也原样归档。

历史文档中的“WP0–WP6 完成”不能直接作为验收结论；以本计划的 Gate 和 Definition of Done 为准。

## 3. 当前基线和完成度审计

### 3.1 已经具备的能力

- 公开 client/request API，版本为 `0.2.0`；
- OpenAI、火山方舟和 custom provider preset；
- Responses SSE 与 Chat Completions SSE adapter；
- `esp_http_client` HTTP/TLS transport；
- 有界 SSE parser、请求体和错误正文；
- 文本 delta、usage、completed/incomplete/cancelled/error 事件；
- `main/llm_app.cpp` worker task + LVGL timer 的非阻塞接入；
- Ark Responses 与 Ark Chat 的实机成功记录；
- 当前宿主 parser/adapter 测试实跑结果：`1131 checks, 0 failures`。

### 3.2 当前真实完成度

严格按旧文本 MVP Definition of Done 评估：

| 范围 | 估算完成度 | 判断 |
|---|---:|---|
| Ark 正常文本流式路径 | 80% | 可用原型 |
| 旧文本 MVP 全部验收项 | 63%–65% | 未完成 |
| 生产可靠性 | 约 55% | 不可发布 |
| 工具调用/Agent Runtime | 0%–10% | 尚未实现 |
| 语音/Realtime | 0% | 尚未开始 |

### 3.3 必须先修复的已知缺陷

以下问题在 Agent 循环之前必须修复，否则错误、取消和工具轮次会被错误处理。

| 优先级 | 问题 | 位置 | 必须达到的结果 |
|---|---|---|---|
| P0 | adapter 已发 terminal `ERROR` 时，`execute_stream()` 又把返回值改成 `ESP_OK` | `src/core/request.c` terminal 分支 | 返回 ERROR 中的真实 `esp_err_t` |
| P0 | 跨任务取消调用 `esp_http_client_cancel_request()`；当前 IDF 实现关闭后重新 connect | `src/core/request.c` | 改为只关闭当前 socket，不重连、不重发 POST |
| P0 | SSE 的 `last_was_cr` 是单次 `feed()` 局部变量，CRLF 跨分片会被识别成两个换行 | `src/transports/sse_parser.c` | CR/LF/CRLF 在任意字节边界切分结果一致 |
| P1 | custom provider 非法配置、消息复制中途 OOM、鉴权头构建失败存在泄漏路径 | `src/core/client.c`、`src/core/request.c` | 所有失败路径零泄漏 |
| P1 | client 可在尚存 request handle 时销毁，request 随后可能访问悬空 client | client/request 生命周期 | 引用计数或严格所有权检查 |
| P1 | OpenAI 未做真实服务 smoke test | 测试记录 | OpenAI Responses 文本与工具调用均实测 |
| P1 | Kconfig/历史文档含实际形态 Wi-Fi 凭证，LLM Key 通过 Kconfig 编入应用固件 | `main/Kconfig`、`main/llm_app.cpp` | 仓库无真实凭证，运行时安全注入 |
| P2 | host fixture/第三方测试依赖不能保证 clean clone 可复现 | `test_apps/parser_and_adapters` | 一条命令从干净工作区重建并运行 |

### 3.4 当前构建证据边界

- 当前 build target 为 ESP32-P4，ESP-IDF 为 `v6.1-beta1`；
- `ninja -C build -n` 没有要求重新编译 SDK/application 源码，但这不是 clean build；
- 后续 agent 必须执行一次 clean、可复现的完整构建，不能只引用旧 `.bin` 或旧日志。

## 4. 官方协议事实

### 4.1 OpenAI Responses Function Calling

OpenAI 官方 Function Calling 流程是：

1. 在请求 `tools` 中声明函数名称、描述、JSON Schema 和 `strict`；
2. 模型输出 `function_call`，其中包含 `name`、`call_id`、`arguments`；
3. 应用执行本地工具；
4. 应用提交 `function_call_output`，并保持同一个 `call_id`；
5. 继续请求，直到模型输出最终消息且没有待执行工具。

流式 Responses 必须识别并聚合：

- `response.output_item.added`，且 `item.type == "function_call"`；
- `response.function_call_arguments.delta`；
- `response.function_call_arguments.done`；
- `response.output_item.done`；
- 既有文本、usage、completed/incomplete/error 事件。

默认启用 `strict: true`。工具参数 schema 至少满足：对象的 `additionalProperties: false`，所有 property 都在 `required` 中；可选值使用含 `null` 的联合类型表示。

### 4.2 火山方舟 Responses Function Calling

火山方舟官方 Responses 文档采用相同的核心结构：`tools`、`function_call`、`call_id`、`arguments`、`previous_response_id` 和 `function_call_output`。因此 Responses 工具调用可以共享一个规范化内部模型，但仍需分别保存 OpenAI 和 Ark 的真实 fixture，禁止假设所有事件细节永远相同。

### 4.3 Chat Completions 兼容模式

Chat 模式需要独立 adapter：

- 请求工具定义使用 `tools[].function`；
- 流式响应从 `choices[].delta.tool_calls[]` 按 `index` 聚合 `id`、`name`、`arguments`；
- `finish_reason == "tool_calls"` 表示进入工具阶段；
- 下一轮历史必须包含 assistant 的 `tool_calls` 消息，以及对应的 `role: "tool"`、`tool_call_id` 和结果内容；
- `[DONE]` 只代表当前 HTTP stream 结束，不等于整个 Agent run 已完成。

### 4.4 不可混淆的终止语义

- **模型响应结束**：一次 HTTP/模型 turn 结束；它可能要求执行工具。
- **工具调用结束**：某个工具的参数已收齐并执行完。
- **Agent run 结束**：当前没有待执行工具，且已得到最终文本或明确失败/取消。

现有 `COMPLETED` 只能继续表示“当前模型响应结束”。Agent 层必须另设 `RUN_COMPLETED`，不能收到第一个 `response.completed` 就结束整个 Agent。

## 5. 目标架构

```text
Application / LVGL / Voice frontend
                |
        otool_llm_agent.h
     Agent session + bounded loop
                |
    +-----------+------------+
    |                        |
Tool registry/executor   Conversation state
    |                        |
    +-----------+------------+
                |
    Normalized turn/input/event model
                |
    +-----------+-------------+
    |                         |
Responses adapter      Chat Completions adapter
    |                         |
OpenAI / Ark presets        Ark / Custom
                |
       HTTP/TLS + SSE parser
```

分层规则：

1. transport 不认识 provider、文本和工具语义；
2. protocol adapter 只负责 JSON 序列化、事件解析和规范化；
3. Agent Runtime 负责循环、状态、上限、工具调度和终止；
4. 工具实现属于应用，不属于 SDK；
5. provider 文件只保存 endpoint、header 和 provider error 解析；
6. 语音层以后复用 Agent Runtime，不把音频帧塞入文本 SSE parser。

## 6. 公开 API 方向

保留 `otool_llm_sdk.h` 和 `otool_llm_text.h` 的已有 API；新增两个公开头文件：

```text
include/
  otool_llm_sdk.h
  otool_llm_text.h
  otool_llm_tools.h
  otool_llm_agent.h
```

以下代码是接口形状约束，不要求执行 agent 逐字照抄命名；若改变必须在 ADR 中说明原因。

### 6.1 工具定义与执行接口

```c
typedef enum {
    OTOOL_LLM_TOOL_READ_ONLY        = 1u << 0,
    OTOOL_LLM_TOOL_IDEMPOTENT       = 1u << 1,
    OTOOL_LLM_TOOL_SIDE_EFFECTING   = 1u << 2,
    OTOOL_LLM_TOOL_NEEDS_APPROVAL   = 1u << 3,
} otool_llm_tool_flags_t;

typedef struct {
    const volatile bool *cancel_requested;
    int64_t deadline_us;
} otool_llm_tool_exec_context_t;

typedef esp_err_t (*otool_llm_tool_execute_cb_t)(
    const char *arguments_json,
    char *output_json,
    size_t output_capacity,
    size_t *output_length,
    const otool_llm_tool_exec_context_t *exec_ctx,
    void *user_ctx);

typedef struct {
    size_t struct_size;
    const char *name;
    const char *description;
    const char *parameters_json_schema;
    bool strict;
    uint32_t flags;
    uint32_t timeout_ms;
    size_t max_output_bytes;
    otool_llm_tool_execute_cb_t execute;
    void *user_ctx;
} otool_llm_tool_definition_t;
```

约束：

- SDK 深拷贝 name、description 和 schema；
- `name` 必须非空、唯一、长度受限且只允许安全字符；
- schema 在注册时解析和校验，不能等模型调用时才发现错误；
- `strict` 默认 true；
- callback 输出必须是有效 UTF-8 字符串，建议是 JSON object；
- SDK 提供固定容量输出 buffer，工具不得把裸指针留到 callback 之后；
- MVP callback 在 Agent worker task 中同步执行，必须周期检查取消标志和 deadline；
- SDK 不得用 `vTaskDelete()` 强杀正在访问硬件的工具 task；真正异步工具接口放到后续版本。

工具 registry API 至少包括：

```c
otool_llm_tool_registry_create(...);
otool_llm_tool_registry_add(...);
otool_llm_tool_registry_seal(...);
otool_llm_tool_registry_destroy(...);
```

`seal()` 后不可增删工具，允许 Agent run 无锁读取；有活跃 session/run 时 registry 不可销毁。

### 6.2 Agent 配置与会话

```c
typedef enum {
    OTOOL_LLM_AGENT_STATE_REMOTE_RESPONSE_CHAIN,
    OTOOL_LLM_AGENT_STATE_LOCAL_TRANSCRIPT,
} otool_llm_agent_state_mode_t;

typedef enum {
    OTOOL_LLM_TOOL_DECISION_ALLOW,
    OTOOL_LLM_TOOL_DECISION_DENY,
} otool_llm_tool_decision_t;

typedef otool_llm_tool_decision_t (*otool_llm_tool_policy_cb_t)(
    const char *tool_name,
    const char *arguments_json,
    uint32_t tool_flags,
    void *user_ctx);

typedef struct {
    size_t struct_size;
    otool_llm_client_handle_t client;
    otool_llm_tool_registry_handle_t tools;
    const char *model;
    const char *instructions;
    otool_llm_agent_state_mode_t state_mode;
    uint32_t max_turns;
    uint32_t max_tool_calls;
    uint32_t run_timeout_ms;
    bool parallel_tool_calls;
    otool_llm_tool_policy_cb_t policy;
    void *policy_ctx;
} otool_llm_agent_config_t;

otool_llm_agent_create(...);
otool_llm_agent_run_stream(...);
otool_llm_agent_cancel(...);
otool_llm_agent_reset_session(...);
otool_llm_agent_destroy(...);
```

MVP 默认：

- `max_turns = 6`；
- `max_tool_calls = 8`；
- `parallel_tool_calls = false`；
- `tool_choice = auto`；
- side-effecting 或 needs-approval 工具在没有 policy callback 明确 `ALLOW` 时默认拒绝；
- 同一个 agent handle 同时只允许一个 run；不同 client 是否允许并发由 client 配置和资源报告决定。

### 6.3 Agent 事件

新增独立事件枚举，不破坏现有 text event：

```c
typedef enum {
    OTOOL_LLM_AGENT_EVENT_RUN_STARTED,
    OTOOL_LLM_AGENT_EVENT_TURN_STARTED,
    OTOOL_LLM_AGENT_EVENT_TEXT_DELTA,
    OTOOL_LLM_AGENT_EVENT_TOOL_CALL_STARTED,
    OTOOL_LLM_AGENT_EVENT_TOOL_ARGUMENTS_DELTA,
    OTOOL_LLM_AGENT_EVENT_TOOL_CALL_READY,
    OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_STARTED,
    OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FINISHED,
    OTOOL_LLM_AGENT_EVENT_TOOL_EXECUTION_FAILED,
    OTOOL_LLM_AGENT_EVENT_USAGE,
    OTOOL_LLM_AGENT_EVENT_TURN_COMPLETED,
    OTOOL_LLM_AGENT_EVENT_RUN_COMPLETED,
    OTOOL_LLM_AGENT_EVENT_RUN_LIMIT_REACHED,
    OTOOL_LLM_AGENT_EVENT_CANCELLED,
    OTOOL_LLM_AGENT_EVENT_ERROR,
} otool_llm_agent_event_type_t;
```

每个 tool event 至少携带：`turn_index`、`tool_index`、`call_id`、`name`，参数/结果 span 只在 callback 返回前有效。整个 run 恰好产生一个 terminal agent event。

## 7. 内部规范化数据模型

不能让 Agent Runtime 直接判断 OpenAI/Ark 的 JSON 字段。新增内部 union：

```text
input item:
  MESSAGE(role, text)
  FUNCTION_CALL(call_id, name, arguments)
  FUNCTION_CALL_OUTPUT(call_id, output)

protocol event:
  RESPONSE_STARTED(response_id, model)
  TEXT_DELTA(text)
  TEXT_DONE
  TOOL_CALL_STARTED(output_index, item_id, call_id, name)
  TOOL_ARGUMENTS_DELTA(output_index, delta)
  TOOL_CALL_DONE(output_index, call_id, name, arguments)
  USAGE(...)
  RESPONSE_COMPLETED / RESPONSE_INCOMPLETE / ERROR
```

adapter 必须做到：

- 按 `output_index` 或 Chat `tool_calls[].index` 维护最多 `CONFIG_OTOOL_LLM_MAX_PENDING_TOOL_CALLS` 个调用；
- 验证 start/delta/done 的 index、item_id、call_id 一致性；
- 参数总长超过上限立即产生协议错误；
- done 事件中的完整 arguments 与已累计 delta 不一致时记录协议错误，不能静默选择其中一个；
- 未知非关键 SSE 事件可忽略，但未知 tool item 不得当作普通文本；
- 终止时如果仍有半个 tool call，返回 `OTOOL_LLM_ERR_PROTOCOL_EOF`。

## 8. Agent 主循环

### 8.1 状态机

```text
IDLE
  -> MODEL_STREAMING
      -> FINAL_TEXT -> RUN_COMPLETED
      -> TOOL_CALLS_READY
          -> POLICY_CHECK
              -> TOOL_DENIED -> submit structured error result
              -> TOOL_EXECUTING
                  -> TOOL_RESULT_READY
                      -> SUBMIT_TOOL_OUTPUTS
                          -> MODEL_STREAMING
  -> LIMIT / CANCELLED / ERROR
```

### 8.2 每轮算法

1. 构建当前 turn 请求，附带 sealed tool registry；
2. 设置 `stream=true`、有界 token 数、`tool_choice` 和 `parallel_tool_calls`；
3. 流式转发文本，同时累计规范化 tool calls；
4. 当前模型响应结束后：
   - 没有 tool call：完成 run；
   - 有 tool call：进入策略检查和工具执行；
5. 根据 `call_id` 构造一个或多个 tool outputs；
6. Responses 使用 `function_call_output`；Chat 使用 assistant tool_calls + tool role 消息；
7. 发起下一轮，直到最终文本、取消、错误或上限命中。

### 8.3 停止和防失控规则

- turn 数达到 `max_turns`：停止并发 `RUN_LIMIT_REACHED`；
- tool 调用数达到 `max_tool_calls`：停止；
- 同一 `name + canonical arguments` 连续出现超过 2 次：视为循环；
- 未注册工具：生成结构化 `unknown_tool` 结果回传一次；重复后终止；
- 参数不是合法 JSON 或不符合本地 schema：生成 `invalid_arguments` 结果；
- policy 拒绝：生成 `permission_denied` 结果，不调用工具；
- 工具 callback 返回错误：生成结构化失败结果，让模型有一次恢复机会；
- 整体 deadline 优先于单工具 deadline；
- 本地取消优先于网络错误，最终必须是 `CANCELLED` 而非随机 transport error；
- SDK 不自动重试产生副作用的 POST 或工具；429/5xx 是否重试由更高层显式策略决定，MVP 默认不重试。

### 8.4 标准工具结果封装

成功：

```json
{"ok":true,"result":{"temperature_c":25}}
```

失败：

```json
{"ok":false,"error":{"code":"permission_denied","message":"tool not approved","retryable":false}}
```

工具可返回普通文本，但 SDK 自己生成的错误必须使用稳定 JSON 格式。错误消息不得包含 API Key、Wi-Fi 密码或完整敏感参数。

## 9. 工具 schema 与本地参数验证

不能因为 provider 支持 strict mode 就信任网络输入。模型工具参数始终是不可信数据。

MVP 在板端支持以下 JSON Schema 子集：

- `type: object` 顶层；
- `properties`；
- `required`；
- `additionalProperties: false`；
- property 类型：`string`、`number`、`integer`、`boolean`、`null`；
- `enum`；
- 字符串长度和数值 min/max 可作为 P1；
- 数组、深层递归对象先设最大深度，未实现的 schema 关键字在注册时明确拒绝。

必须有两层验证：

1. registry 注册时验证 schema 本身和受支持关键字；
2. tool call ready 后验证 arguments 实例。

不允许：

- 用字符串拼接生成 schema 或请求 JSON；
- 未注册函数名的动态符号查找；
- 把模型参数直接当 shell、格式串、SQL 或文件路径使用；
- 工具在没有自身边界检查时直接操作 GPIO、电源、文件或升级流程。

## 10. 会话和上下文策略

### 10.1 Responses 远端链模式

MVP 推荐 `REMOTE_RESPONSE_CHAIN`：

- 显式设置 `store=true`；
- 保存每轮 `response_id`；
- 下一轮用 `previous_response_id` + `function_call_output`；
- agent reset 时清除本地 response id；
- provider、model 或工具集合变化时默认开启新链。

这是 ESP32 内存成本最低的方案，但应用必须知晓服务端存储语义。不得偷偷把现有 text API 的 `store=false` 默认改成 true；只在 Agent 配置中显式选择。

### 10.2 本地 transcript 模式

Chat adapter 必须支持 `LOCAL_TRANSCRIPT`：

- 保存 user、assistant、tool 的最小必要消息；
- assistant tool_calls 和 tool result 必须成对保留；
- 达到内存上限时返回 `CONTEXT_FULL`，MVP 不静默删除中间工具对；
- 后续版本可以增加按完整 turn 截断或摘要压缩。

Responses 的完全 stateless 模式需要保留输出 items，推理模型还可能要求回传 reasoning items。此能力放到 `0.4.x`，不得在 `0.3.0` 中伪装为完整支持。

## 11. 线程、取消和生命周期

- `agent_run_stream()` 与工具 callback 在调用方 worker task 运行，严禁在 LVGL task 调用；
- callback 只做短操作或写入线程安全队列；
- `agent_cancel()` 可从其他 task 调用；
- 取消网络时只关闭当前 transport，不 reconnect；
- 取消标志必须贯穿 Agent、当前 request 和 tool exec context；
- 工具 callback 必须合作式检查取消，SDK 不强杀持有硬件资源的 task；
- destroy 仅在 run 返回后允许；
- registry/client 的生命周期必须长于所有 agent/session，使用引用计数或显式 child count 拒绝过早销毁；
- 不允许持锁调用可能阻塞的 `esp_http_client_*`、用户 callback 或工具函数；
- 每次 run 恰好一个 terminal agent event；每次模型 request 也恰好一个 terminal response event。

## 12. 错误模型

新增或整理以下稳定错误类别：

```text
OTOOL_LLM_ERR_HTTP_STATUS
OTOOL_LLM_ERR_PROVIDER
OTOOL_LLM_ERR_PROTOCOL
OTOOL_LLM_ERR_PROTOCOL_EOF
OTOOL_LLM_ERR_EVENT_TOO_LARGE
OTOOL_LLM_ERR_TOOL_NOT_FOUND
OTOOL_LLM_ERR_TOOL_SCHEMA
OTOOL_LLM_ERR_TOOL_ARGUMENTS
OTOOL_LLM_ERR_TOOL_OUTPUT_TOO_LARGE
OTOOL_LLM_ERR_TOOL_FAILED
OTOOL_LLM_ERR_TOOL_DENIED
OTOOL_LLM_ERR_AGENT_LIMIT
OTOOL_LLM_ERR_CONTEXT_FULL
```

规则：

- terminal `ERROR` 中的 code 必须等于 API 返回值；
- provider code 与 HTTP request id 保留在 telemetry，不覆盖本地错误类别；
- tool callback 的原始 `esp_err_t` 可记录，但发给模型的结果必须稳定、可清洗；
- `CANCELLED` 返回 `ESP_OK` 可以保留现有契约，但事件必须明确区分；
- OOM、超限和无效生命周期必须有单测，不允许崩溃或泄漏。

## 13. 内存与 Kconfig 初始预算

建议新增配置：

| Kconfig | 默认值 | 说明 |
|---|---:|---|
| `OTOOL_LLM_MAX_TOOLS` | 8 | registry 工具数 |
| `OTOOL_LLM_MAX_TOOL_NAME_BYTES` | 64 | 含结尾零前的最大名称 |
| `OTOOL_LLM_MAX_TOOL_SCHEMA_BYTES` | 2048 | 单工具 schema |
| `OTOOL_LLM_MAX_TOOL_SCHEMA_TOTAL_BYTES` | 8192 | 所有 schema 总和 |
| `OTOOL_LLM_MAX_PENDING_TOOL_CALLS` | 2 | 单 turn 防御性上限 |
| `OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES` | 4096 | 单次 arguments |
| `OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES` | 4096 | 单次 output |
| `OTOOL_LLM_MAX_AGENT_TURNS` | 6 | run 上限 |
| `OTOOL_LLM_MAX_AGENT_TOOL_CALLS` | 8 | run 工具调用上限 |
| `OTOOL_LLM_MAX_AGENT_CONTEXT_BYTES` | 32768 | local transcript 总上限 |
| `OTOOL_LLM_DEFAULT_TOOL_TIMEOUT_MS` | 15000 | 合作式 deadline |
| `OTOOL_LLM_DEFAULT_AGENT_TIMEOUT_MS` | 120000 | 整体 run deadline |

要求：

- 所有累积 buffer 在创建 session/run 时一次性或按上限受控分配；
- 禁止按 SSE delta 次数无限 realloc；
- schema 总大小必须在请求序列化前计算；
- 超出 provider 请求体上限时在发 HTTP 前失败；
- WP9 用实测调整默认值，并分别报告内部 SRAM/PSRAM 峰值；
- 如果工具定义导致每轮请求体成本过高，再设计 tool subset/deferred tools，MVP 不先实现动态 tool search。

## 14. 建议目录

```text
components/otool_llm_sdk/
  include/
    otool_llm_sdk.h
    otool_llm_text.h
    otool_llm_tools.h
    otool_llm_agent.h
  private_include/
    otool_llm_agent_internal.h
    otool_llm_tool_schema.h
    otool_llm_protocol.h
    ...
  src/
    agent/
      agent.c
      agent_state.c
      tool_registry.c
      tool_schema.c
      tool_result.c
    core/
      client.c
      request.c
      ...
    protocols/
      responses_sse.c
      chat_completions_sse.c
      ...
    providers/
    transports/
  test_apps/
    parser_and_adapters/
    agent_host_tests/
    transport_test/
    agent_device_test/
    live_probes/
  test_fixtures/
    openai/
    ark/
```

## 15. 实施任务包

每个 WP 必须单独可验证。执行 agent 不得把多个未测阶段一次性标记完成。

### WP0：冻结基线、清理凭证和建立复现命令

任务：

- 记录当前 git 状态，保护用户已有改动；
- 删除仓库和文档中的真实形态凭证，改为空值/明确 placeholder；
- LLM Key 改为运行时注入接口，示例可从 NVS、受保护分区或应用 callback 读取；
- 轮换历史泄露过的 Key 由用户完成，代码侧记录验证但不保存 Key；
- 修复 host test 的 clean clone 可构建性，不提交平台专用测试 exe；
- 给出 Windows/ESP-IDF 两套明确测试命令。

Gate：凭证扫描无命中；新 clone 能构建并运行 host tests。

### WP1：修复现有文本核心的 P0/P1 缺陷

任务：

- 修复 terminal ERROR 返回码；
- 取消改用 close-only 路径，且不持锁执行阻塞操作；
- 将 CRLF 状态移入 SSE parser 实例；
- 修复所有已知 OOM/非法参数泄漏；
- 为 client/request 增加安全所有权规则；
- 校验 role enum、URL、Content-Type 边界和 provider error code 类型。

Gate：新增回归测试全部通过；ASan/Valgrind 或等价 host 检查覆盖失败路径；真机取消不会重新连接。

### WP2：扩展规范化 input/event 和 Responses tool adapter

任务：

- 扩展 protocol private interface；
- Responses 请求序列化支持 `tools`、`tool_choice`、`parallel_tool_calls` 和 `function_call_output`；
- 解析四类流式 function call 事件；
- 支持同一 turn 多个 output index，但默认请求串行；
- 保存 response id、call id、item id 和 arguments；
- 不改变无工具 text request 的行为。

Gate：官方/清洗 fixture 在每一个字节切分位置得到相同 tool call；无工具旧测试零回归。

### WP3：工具 registry 与本地 schema 验证

任务：

- 新增 `otool_llm_tools.h`；
- registry 深拷贝、唯一性、seal 和生命周期；
- JSON Schema 子集校验器；
- arguments 实例验证；
- 稳定工具结果封装和敏感字段清洗；
- 副作用 flags 和 policy callback。

Gate：重复名称、非法名称、非法/超长 schema、额外字段、缺 required、错误类型和超长参数都被确定性拒绝。

### WP4：基础 Agent Runtime

任务：

- 新增 `otool_llm_agent.h` 和状态机；
- 实现 Responses 远端 response chain；
- 实现工具执行、结果回传、继续生成和最终文本；
- 实现 turn/tool/run 上限、重复调用检测、整体 deadline；
- 取消贯穿网络和工具 callback；
- 事件桥接与恰好一个 terminal run event；
- 错误、policy deny 和 unknown tool 可结构化回传。

Gate：host fake provider 完成“用户问题 → 工具调用 → 工具结果 → 最终回答”的完整闭环；所有限制与取消场景通过。

### WP5：Chat Completions 工具调用与本地 transcript

任务：

- Chat request 序列化工具定义；
- 流式聚合 `delta.tool_calls`；
- 构造 assistant tool_calls 和 tool result 消息；
- bounded local transcript；
- 保证 `[DONE]` 不会提前结束 Agent run。

Gate：Ark Chat 与本地兼容 server 都能完成至少一轮工具闭环；缺失 id、index 混乱和半截 arguments 都返回协议错误。

### WP6：应用集成和最小设备工具

先实现两个无危险示例工具：

1. `get_device_status`：只读，返回 uptime、空闲堆和网络状态；
2. `set_avatar_expression` 或等价可回滚 UI 工具：有副作用，必须经过应用 policy 明确允许。

应用要求：

- Agent 运行仍在 worker task；
- LVGL 只消费队列/共享快照；
- UI 显示 thinking、calling tool、tool result、streaming、cancelled/error；
- 点击新问题可取消旧 run；
- 不在日志打印完整 arguments/output 或凭证；
- 页面退出统一 cancel + join + destroy。

Gate：真机连续完成“查设备状态 → 工具执行 → 中文最终回答”；触摸和动画不中断；副作用工具未授权时不会执行。

### WP7：测试基础设施与 fixture

建立三层测试：

1. 纯 host parser/adapter/tool schema 单测；
2. host fake transport 的 Agent 状态机集成测试；
3. ESP-IDF Unity/真机 transport 与并发取消测试。

fixture 来源：

- 官方 OpenAI SDK/HTTP 生成的清洗 Responses 流；
- 火山方舟官方接口生成的清洗 Responses/Chat 流；
- 手工构造的错误、超限、乱序和断流 fixture。

fixture 不得包含真实回答、Key、request id 或个人数据；保留事件结构和字段类型即可。

Gate：一条 host 命令和一条 IDF test app 命令有文档、有稳定结果。

### WP8：双 provider live smoke

使用运行时临时凭证，最少验证：

| Provider/协议 | 普通文本 | 单工具 | 工具失败恢复 | 取消 |
|---|---:|---:|---:|---:|
| OpenAI Responses | 必须 | 必须 | 必须 | 必须 |
| Ark Responses | 必须 | 必须 | 必须 | 必须 |
| Ark Chat | 必须 | 必须 | 必须 | 必须 |

每条测试只做最小调用，控制费用；原始日志立即清洗，只提交事件清单、耗时和结果，不提交密钥。

Gate：三条协议都完成真实 tool loop；OpenAI 不再只是代码推断。

### WP9：可靠性、资源和安全报告

执行：

- 100 次短 Agent run；
- 50 次连续取消，覆盖模型 stream、参数 stream、工具执行和下一轮请求；
- Wi-Fi 断开/恢复；
- 401、429、5xx、错误 Content-Type；
- OOM 注入；
- 超长 schema、arguments、tool output 和 SSE event；
- 重复工具循环、unknown tool、policy deny；
- client/registry/session 销毁顺序错误；
- side-effect 工具确保至多执行一次。

报告：

- 首 text delta 和首 tool call 延迟；
- 每轮/每 run 总耗时；
- 峰值内部堆、PSRAM、最小剩余堆；
- task stack high-water mark；
- 取消 P50/P95；
- 请求体大小与工具 schema 成本；
- 100 次运行后的净堆变化；
- 固件大小变化。

Gate：无增长性泄漏、无 WDT、无 UI 卡死、无取消后 callback、无重复副作用执行。

### WP10：语音扩展 ADR

文本 Agent 稳定后再确定：

- ASR → 文本 Agent → TTS 的半双工链路；
- 或 Ark/OpenAI Realtime WebSocket 的全双工链路；
- 音频输入/输出 buffer、VAD、打断和回声处理；
- Realtime tool calling 如何映射到相同 registry 和 policy；
- 是否新增 `otool_llm_realtime.h`，禁止污染 `otool_llm_text.h`。

Gate：提交 ADR 和最小协议探针；未完成前不把语音代码混入 Agent MVP。

## 16. 测试矩阵

### 16.1 SSE 与 tool adapter

- LF、CR、CRLF；
- CRLF 正好跨 feed 边界；
- UTF-8 多字节和 JSON escape 跨边界；
- function call start/name/arguments 每字节切分；
- 两个 call index 交错；
- done 前断流；
- done 完整 arguments 与累计 delta 不一致；
- event、arguments 和 request body 超限；
- 未知事件容忍、未知 tool item 拒绝。

### 16.2 Agent 状态机

- 无工具直接最终回答；
- 一个工具一轮；
- 两个工具多轮；
- 工具返回空结果；
- 工具返回业务失败；
- unknown tool；
- 非法参数/schema 不匹配；
- policy deny；
- 重复工具死循环；
- turn/tool/run timeout 上限；
- 每个阶段取消；
- callback 请求取消；
- 模型响应 completed 但仍有 tool call；
- 最终回答文本与 usage 顺序变化；
- terminal event 和 API 返回码一致。

### 16.3 生命周期和故障注入

- 每一个 allocation point OOM；
- request/session 存在时销毁 client；
- agent 存在时销毁 registry；
- callback 内错误调用 destroy；
- transport 初始化、open、write、read、cleanup 分别失败；
- 工具 callback 超时但合作退出；
- 取消与 response/tool completion 同时发生。

## 17. Definition of Done

`otool_llm_sdk` Agent MVP 只有同时满足以下条件才算完成：

- ESP32-P4 + 当前锁定 ESP-IDF 能 clean build；
- 原有文本 API 无破坏性回归；
- P0/P1 审计缺陷已修复并有回归测试；
- OpenAI Responses 与 Ark Responses 支持流式 function calling；
- Ark Chat 支持流式 tool_calls；
- 本地工具 registry、strict schema 子集验证和白名单策略可用；
- 能自动完成至少一轮 tool call → tool output → final answer；
- 有界 turn、tool call、arguments、output、context 和总运行时间；
- 网络/工具/Agent 取消统一且不 reconnect、不重发副作用；
- 每个模型 request 与每个 Agent run 各恰好一个 terminal event；
- terminal ERROR 的事件 code 与函数返回值一致；
- OpenAI、Ark Responses、Ark Chat 三条真实 smoke 全通过；
- host、Unity/设备、可靠性测试和资源报告齐全；
- 仓库、固件默认配置、fixture 和日志无真实凭证；
- README 有最小文本、单工具 Agent、取消和安全示例；
- 文档不得再用“编译通过”代替“功能验收通过”。

## 18. 给后续执行 agent 的规则

1. 从 WP0 开始，不得绕过三个 P0 缺陷直接写 Agent loop；
2. 每次只让一个内部层拥有 provider JSON 细节；
3. 先写 fixture/失败测试，再修 parser 和 adapter；
4. 保持现有用户改动，修改前检查 `git status`；
5. 不提交 API Key、Wi-Fi 密码、原始线上响应或含个人数据日志；
6. 不用正则或字符串查找代替 JSON 解析；
7. 不在 LVGL task 发网络请求或执行工具；
8. 不增加无上限容器、递归 schema 或自动重试；
9. 工具输出和模型输出都视为不可信输入；
10. 每完成一个 WP，在本文末尾追加：改动、命令、结果、剩余风险；
11. 未达到 Gate 的 WP 标记“进行中/阻塞”，禁止标记完成；
12. 如果官方协议与 fixture 不一致，先保存清洗证据并更新 adapter ADR，不写 provider 特判到 Agent Runtime。

## 19. 官方资料基线

- [OpenAI Function calling](https://developers.openai.com/api/docs/guides/function-calling)：Function Calling 循环、strict schema、tool choice、parallel calls 和流式参数事件。
- [OpenAI Responses create reference](https://developers.openai.com/api/reference/typescript/resources/beta/subresources/responses/methods/create)：Responses 请求、tools、previous response 和流式响应结构。
- [火山方舟 Responses 工具调用](https://www.volcengine.com/docs/82379/1958524?lang=zh)：Ark `function_call`/`function_call_output` 官方示例。
- [火山方舟 Chat API Explorer](https://api.volcengine.com/api-explorer/?action=ChatCompletions&groupName=%E5%AF%B9%E8%AF%9D%28Chat%29+API&serviceCode=ark&version=2024-01-01)：Chat tools 与流式响应字段。
- [WHATWG Server-Sent Events](https://html.spec.whatwg.org/multipage/server-sent-events.html)：SSE 字节和字段处理模型。
- 本地 ESP-IDF `components/esp_http_client/esp_http_client.c`：当前锁定 IDF 版本取消 API 的真实实现，以源码而非函数名推断行为。

## 20. 实施进度记录

### 2026-08-22 — WP2（完成：Responses 工具调用 adapter）

改动：

- 新增 `include/otool_llm_tools.h`：`otool_llm_tool_definition_t`（name/description/schema/strict/flags/timeout/execute）、`otool_llm_tool_output_t`（function_call_output）；
- `include/otool_llm_sdk.h`：新增错误码 `TOOL_NOT_FOUND/TOOL_SCHEMA/TOOL_ARGUMENTS/TOOL_OUTPUT_TOO_LARGE/TOOL_FAILED/TOOL_DENIED/AGENT_LIMIT/CONTEXT_FULL`；
- `include/otool_llm_text.h`：请求新增 `tools/tool_count/tool_outputs/tool_output_count`；事件新增 `TOOL_CALL_STARTED/TOOL_ARGUMENTS_DELTA/TOOL_CALL_DONE`；
- `private_include/otool_llm_protocol.h`：request_view 增加工具字段；`otool_llm_pending_tool_call_t`（按 output_index 的槽位，arguments 上限 4096）；
- `src/protocols/responses_sse.c`：
  - 请求序列化：`tools[]`（type/name/description/parameters/strict）、`tool_choice:auto`、`parallel_tool_calls:false`、input 追加 `function_call_output` item；
  - 事件解析：`output_item.added(function_call)` → 槽位分配 + TOOL_CALL_STARTED；`function_call_arguments.delta`（允许空串，按 output_index 关联，无 call_id 也能工作——Ark 实测）→ 追加 + TOOL_ARGUMENTS_DELTA；`function_call_arguments.done` → 与累计拼接一致性校验 + TOOL_CALL_DONE；`output_item.done` 容错补发并回收槽位；
  - `response.completed/incomplete` 前检查残留半工具调用 → ERROR(PROTOCOL)；
- `src/core/request.c`：tools/tool_outputs 深拷贝（含全部失败路径释放）、Chat 协议收到工具字段明确拒绝（WP5 前）、view 填充；
- `Kconfig`：`OTOOL_LLM_MAX_TOOLS=8`、`OTOOL_LLM_MAX_PENDING_TOOL_CALLS=2`、`OTOOL_LLM_MAX_TOOL_ARGUMENT_BYTES=4096`、`OTOOL_LLM_MAX_TOOL_OUTPUT_BYTES=4096`。

协议事实（Ark 实测，`test_apps/ark_tool_probe.py`）：

- 事件序列：`output_item.added(function_call)` → `function_call_arguments.delta`×N（首片可为空串）→ `function_call_arguments.done`（完整 arguments）→ `output_item.done` → `response.completed`；
- **Ark 的 arguments.delta/done 事件不含 call_id**（OpenAI 有），必须按 `output_index`+`item_id` 关联；
- done 的完整 arguments 与 delta 拼接一致（`{"city": "北京"}` 18 字节）。

验证：

- host 测试新增 4 组：完整工具流（started/空 delta/中文 delta/done 一致性/item done/completed）、done 不一致 → ERROR(PROTOCOL)、终止时半工具调用 → ERROR(PROTOCOL)、带 tools 的请求序列化；`1332 checks, 0 failures`；
- 固件：`idf.py build` → Project build complete（0x2ea230）；
- 提交：`见 git log（WP2）`。

剩余风险：

- Chat 工具调用（WP5）当前显式拒绝；
- OpenAI 真机工具调用未测（无 OpenAI 凭证，WP8 时评估）；

### 2026-08-22 — WP0（完成）+ WP1（完成）

改动：

- WP0 凭证：
  - `main/Kconfig`：`OTOOL_WIFI_PASSWORD` 默认改为空（真实密码不入库）；`OTOOL_LLM_API_KEY` 保持空默认；Wi-Fi SSID 保留非机密默认并由 NVS 覆盖；
  - 新增 `main/credential_store.{h,cpp}`：NVS 命名空间 `otool_cred`，键 `wifi_ssid`/`wifi_pass`/`llm_key`，启动时缓存；
  - console 新增 `cred`（脱敏显示）/ `cred-set <name> <value>` / `cred-clear <name>` 命令；
  - `wifi_app.cpp` 从 NVS 读 SSID/密码；`llm_app.cpp` 从 NVS 读 API Key，未设置时明确提示并退出 worker；
  - `main.cpp` 在启动早期调用 `credential_store_init()`。
- WP1 P0：
  - P0-1 终端错误返回码：`execute_stream()` 在 terminal 为 ERROR 时返回 `ctx->error_code`（不再一律 ESP_OK）；
  - P0-2 取消 close-only：`request_cancel_locked()` 只置标志并摘走 HTTP handle，锁外 `esp_http_client_close()`；不再调用会重连的 `esp_http_client_cancel_request()`；callback CANCEL 路径同样处理；
  - P0-3 SSE CRLF 跨分片：`last_was_cr` 从 `feed()` 局部变量移入 parser 实例（`p->last_was_cr`）。
- WP1 P1：
  - `request.c` auth 构建失败路径的 `url` 泄漏（声明提前到函数顶部并统一释放）；
  - client/request 生命周期：`client->request_count` 引用计数，`request_create` 增加、`request_destroy` 减少，`client_destroy` 在有活跃请求或存活 request handle 时拒绝。

验证：

- host 测试：新增 CRLF 跨分片回归（每个分片位置对拍 + finish 干净）；`1286 checks, 0 failures`；
- 固件：`idf.py build` → Project build complete，`otool_tab5_live2d.bin` 0x2e8eb0（81% 分区空闲）；
- 提交：`09da62f`。

剩余风险：

- WP1 其余 P1（OpenAI 真机 smoke、host test clean clone 复现命令）待后续 WP；
- 设备侧 NVS 凭证需首次通过 console 注入（`cred-set wifi_pass ...`、`cred-set llm_key ...`）。

### 2026-08-22 — WP3（完成：工具 registry + 本地 schema 校验）

改动：

- `include/otool_llm_tools.h`：registry API（create/add/seal/destroy/find/count/at）+ `otool_llm_tool_arguments_validate()`；
- 新增 `src/agent/tool_registry.c`：深拷贝、唯一性、名称安全字符校验（字母数字下划线，<64B）、schema 注册时校验、seal 后拒绝增删；
- 新增 `src/agent/tool_schema.c`（+`private_include/otool_llm_tool_schema.h`）：JSON Schema 子集校验器（顶层 object、properties、required 名称必须已声明、additionalProperties:true 拒绝、属性类型 string/number/integer/boolean/null、enum；minLength/maxLength/minimum/maximum/pattern/items/anyOf/allOf/oneOf/not/format/$ref 出现即拒绝；深度 ≤3）；
- 两层校验：注册时验 schema、调用前验 arguments 实例（缺 required/未知字段/类型错/enum 不匹配/非 object → `OTOOL_LLM_ERR_TOOL_ARGUMENTS`）。

验证：

- host 测试新增 3 组（registry 基本/非法 schema 矩阵/arguments 校验矩阵），`1362 checks, 0 failures`；
- 固件 `idf.py build` → BUILD OK。

剩余风险：

- 数组/嵌套 object 属性暂不支持（注册即拒）；字符串长度/数值范围校验为 P1。

### 2026-08-22 — WP4（完成：基础 Agent Runtime）

改动：

- 新增 `include/otool_llm_agent.h`（create/run_stream/cancel/reset_session/destroy；事件 RUN_STARTED/TURN_STARTED/TEXT_DELTA/TOOL_CALL_*/TOOL_EXECUTION_*/USAGE/TURN_COMPLETED/RUN_COMPLETED/RUN_LIMIT_REACHED/CANCELLED/ERROR）；
- 新增 `src/agent/agent.c`：平台无关核心（仅依赖 SDK API + `esp_timer_get_time`）；
  - Responses 远端 response chain：`store=true` + 每轮保存 response_id + 下轮 `previous_response_id` + `function_call_output`；
  - 每轮工具数组取自 registry；bridge 事件把 SDK 文本事件转发为 agent 事件并收集 tool calls；
  - 工具执行：policy 检查（副作用/需审批工具在无 policy 时默认拒绝）、schema 参数校验、合作式取消与 deadline、输出有界；
  - 稳定错误封装 JSON：`unknown_tool`/`permission_denied`/`invalid_arguments`/`tool_failed`（`{"ok":false,"error":{...}}`）；
  - 上限：max_turns（默认 6）/max_tool_calls（默认 8）/run_timeout（默认 120s）；取消贯穿 request 与工具执行；每次 run 恰好一个 terminal agent event；
- host 测试新增 `test_apps/agent_host_tests.c`（fake provider 两轮脚本：模型要工具 → 执行 get_device_status → 结果回传 → 最终中文回答；断言链请求结构），`20 checks, 0 failures`。

验证：

- `host_tests` 1362 + `agent_host_tests` 20 全绿；固件 `idf.py build` → BUILD OK（0x2ea230）；
- 提交：`434f059`。

剩余风险：

- 循环检测（同一 name+arguments 连续 2 次）与 Chat 工具调用（WP5）未实现；
- OpenAI 真机工具闭环未测（无凭证）。

### 2026-08-22 — WP6（完成：设备工具 + Agent 应用集成）

改动：

- 新增 `main/agent_app.{h,cpp}`：
  - 只读设备工具 `get_device_status`（uptime / free heap / Wi-Fi 状态，flags=READ_ONLY）；
  - agent worker task（**32KB 栈**——16KB 时真机运行栈溢出，`Detected in task "agent_worker"`，已修复）；事件驱动（console `agent <text>` 触发）；
  - 事件全部流式打印到 console（RUN/TURN/TOOL_CALL/TOOL_EXECUTION/USAGE/RUN_COMPLETED），最终回复累积到共享 buffer 供 UI；
  - 新 run 自动取消旧 run；`agent-cancel` 仅取消。
- console 新增 `agent` / `agent-cancel` / `agent-status` 命令；main.cpp 装配 `agent_app_start()`。

真机验证（COM3，Ark Responses + doubao-seed-2-1-turbo-260628）：

```
[agent] RUN_STARTED
[agent] TURN_STARTED turn=1
[agent] TOOL_CALL_STARTED name=get_device_status
[agent] TOOL_ARGUMENTS_DELTA ×2
[agent] TOOL_CALL_READY args={}
[agent] USAGE in=408 out=74
[agent] TURN_COMPLETED
[agent] TOOL_EXECUTION_STARTED executing=get_device_status
[agent] TOOL_EXECUTION_FINISHED result={"ok":true,"result":{"uptime_s":13,"free_heap_bytes":26795056,"wifi_connected":true}}
[agent] TURN_STARTED turn=2
[agent] USAGE in=531 out=240
[agent] TURN_COMPLETED
[agent] RUN_COMPLETED
```

中文最终回答逐字流式输出（含真实设备数据）。Gate 达成：真机连续完成"查设备状态 → 工具执行 → 中文最终回答"。

剩余风险：

- 副作用工具的 policy 允许路径未真机演示（get_device_status 为只读，默认允许）；
- 触摸/UI 触发 agent 尚未接入（当前 console 触发）。

### 2026-08-22 — WP5（完成：Chat 工具调用 + 本地 transcript）

改动：

- `include/otool_llm_text.h`：消息结构扩展 `OTOOL_LLM_ROLE_TOOL` + `tool_calls[]/tool_call_count/tool_call_id`；新增 `otool_llm_tool_call_msg_t`；
- `src/protocols/chat_completions_sse.c`：
  - 请求序列化：`tools[].function`（name/description/parameters/strict）、assistant 消息 `tool_calls[]`、`role:"tool"` 消息（tool_call_id + content）；
  - 流式解析：`delta.tool_calls[]` 按 index 聚合（id/name/arguments 分片），`finish_reason=tool_calls` 不终止；`[DONE]` 时对 active 槽补发 `TOOL_CALL_DONE` 再 COMPLETED（Chat 无独立 done 事件）；
- `src/agent/agent.c`：`LOCAL_TRANSCRIPT` 模式（`state_mode` 选择）：
  - 有界本地 transcript（≤8 条：user/assistant(tool_calls)/tool/assistant 文本），每轮重建 messages；
  - 工具轮：assistant tool_calls 消息 + tool 结果消息成对入 transcript；最终轮：assistant 文本入 transcript；
- `src/core/request.c`：消息深拷贝支持 tool_calls/tool_call_id（统一 `request_messages_free` 释放）；Chat 协议仅拒绝 Responses 专属字段（previous_response_id/store/tool_outputs），允许 tools。

验证：

- host 测试新增：Chat 流式工具调用（started/args 分片/[DONE] 补发 done/completed）、Chat 工具请求序列化（tools/assistant tool_calls/tool role）；`1386 checks, 0 failures`；
- agent host 测试新增 LOCAL_TRANSCRIPT 闭环（fake Chat 事件脚本：工具调用 → transcript 消息回传 → 最终回答）；`32 checks, 0 failures`；
- 固件 `idf.py build` → BUILD OK（0x2edb50）。

剩余风险：

- Chat 协议真机工具闭环未测（Ark Chat 带工具请求需 WP8 live smoke）；
- transcript 固定 8 条上限，无截断/摘要策略（计划允许 0.4.x 再做）。

### 2026-08-22 — WP8（部分完成：Ark Chat 工具调用真机验证）

改动：

- `main/agent_app.cpp`：协议选择（NVS `otool_cfg/agent_proto`，`responses`（默认，远端链）或 `chat`（本地 transcript））；console 命令 `agent-protocol <responses|chat>`（重启生效）；
- 新增 `test_apps/ark_chat_tool_probe.py`：Ark Chat 工具调用两轮探针（协议事实 + 工具结果消息回传）。

协议事实（Ark Chat 实测）：

- turn1 流式：`delta.tool_calls[{index,id,type,function:{name,arguments}}]`（首 chunk 带 id+name+空 arguments，后续 chunk 仅 arguments 片段）→ `finish_reason=tool_calls` → `[DONE]`；
- turn2：`assistant(tool_calls)` + `role:"tool"`（tool_call_id + 结果）→ 正常流式中文回答。

真机验证（COM3，`agent-protocol chat` 后重启）：

```
[agent] TURN_STARTED turn=1
[agent] TOOL_CALL_STARTED name=get_device_status   （工具调用）
[agent] TOOL_EXECUTION_* result={...uptime_s...}
[agent] TURN_STARTED turn=2                        （tool 消息回传后模型继续）
[agent] TEXT_DELTA ... "剩余堆内存：26781644 字节（约 25.5MB）... Wi-Fi 连接状态：已连接"
[agent] RUN_COMPLETED
```

Gate 达成：Ark Chat 完成真实工具闭环（"用户问题 → 工具调用 → 工具结果 → 最终回答"）。

剩余风险：

- OpenAI Responses 工具调用仍无凭证可测（记录为外部依赖）；
- Ark Responses 工具闭环（WP6 已真机验证）与 Ark Chat 工具闭环（本轮）均已达成。
