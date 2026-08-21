# `otool_llm_sdk` 流式文本与豆包语音扩展实施计划

> 状态：**已决策，待实现**  
> 计划日期：2026-08-22  
> 目标平台：ESP32-P4 / ESP-IDF 6.1 系列  
> 组件位置：`components/otool_llm_sdk`  
> 本文用途：交给后续 agent 直接拆任务、实现和验收；本文不包含本轮代码实现

## 1. 最终决策

保留并重构自研 `components/otool_llm_sdk`，不再用 ESP-Claw 直接替换它。

原因不是 ESP-Claw 无法在本项目编译，而是它当前的 OpenAI-compatible 后端只做非流式 Chat Completions：完整响应先入内存，再返回最终文本，无法满足本项目的逐增量输出；它也没有 OpenAI Responses SSE 事件模型。未来豆包语音还会引入 WebSocket、二进制帧和双向音频，继续把文本协议写死在单文件里会再次返工。

实施结论：

1. 文本 MVP 使用 ESP-IDF 原生 `esp_http_client`、证书包、`espressif/cjson` 和自研有界 SSE 状态机。
2. OpenAI 与火山方舟共用 HTTP/SSE 基础设施，但分别经过“provider preset”和“protocol adapter”，不能以“OpenAI 兼容”为由共用字段假设。
3. 新项目主协议是 Responses API；Chat Completions SSE 保留为方舟旧模型、自定义兼容服务和迁移期后备协议。
4. 文本调用保持阻塞执行语义，由应用放入自己的 worker task；SDK 提供独立 request handle 和跨任务取消，不自行绑定 LVGL 或业务任务模型。
5. 语音不塞进文本 SSE 适配器。以后在同一组件中新增独立的 Voice Session API、WebSocket transport 和豆包语音二进制协议 adapter。
6. SDK 只传输音频帧及元数据，不负责 I2S、麦克风、扬声器、AEC、VAD、重采样或播放器生命周期。

本文是当前权威实施计划。背景调研见：

- [`otool_llm_sdk_integration_plan.md`](./otool_llm_sdk_integration_plan.md)
- [`esp_claw_direct_component_integration_plan.md`](./esp_claw_direct_component_integration_plan.md)

## 2. 已确认的协议事实与不能混淆的边界

### 2.1 文本生成

| 服务 | 默认 base URL | 主协议 | 后备协议 | 鉴权 |
| --- | --- | --- | --- | --- |
| OpenAI | `https://api.openai.com/v1` | `POST /responses` + SSE | `POST /chat/completions` + SSE，仅兼容需要时启用 | `Authorization: Bearer ...` |
| 火山方舟 | `https://ark.cn-beijing.volces.com/api/v3` | `POST /responses` + SSE | `POST /chat/completions` + SSE | `Authorization: Bearer ...` |
| 自定义 OpenAI-compatible 服务 | 运行时传入 | 必须显式选择 | 必须显式选择 | 运行时配置，MVP 仅支持 Bearer |

OpenAI 官方 Responses API 在 `stream: true` 时返回 SSE，文本增量的核心事件是 `response.output_text.delta`，正常终止事件是 `response.completed`。火山方舟官方已经提供 `/api/v3/responses` 和 OpenAI SDK 兼容示例，但不能因此假定它的所有事件、扩展字段和错误体永远与 OpenAI 完全一致；两边必须分别保存脱敏 fixture 并回归。

### 2.2 “豆包语音”不是一个单一接口

未来需求确认前，至少要区分三条产品路线：

| 路线 | 数据流 | 典型传输 | SDK 需要的能力 |
| --- | --- | --- | --- |
| TTS | 文本输入 → 音频输出 | HTTP chunked/SSE 或 WebSocket | 文本分段发送、音频 chunk 回调、格式事件 |
| 级联对话 | 音频 → ASR 文本 → LLM 文本 → TTS 音频 | ASR WebSocket + 本文文本 API + TTS WebSocket | 两条语音 session 和一条文本 request 的编排 |
| 端到端 S2S | 音频输入 ↔ 语音/文本输出 | 持久双向 WebSocket 或 RTC | 双向帧、会话状态、打断、VAD、音频回压 |

豆包语音服务常用 App ID、Access Token、Resource ID 和 `X-Api-*` 请求头；它们与火山方舟的 Ark API Key 不是同一个凭证模型。文本 client config 和未来 voice session config 必须是两个结构体，禁止复用一个 `api_key` 字段硬套。

### 2.3 参考项目的采用边界

| 参考 | 可借鉴 | 不直接采用的原因 |
| --- | --- | --- |
| `volcengine/onesdk@657fceccab227bd860c984cdf37aeae3e68c5b4a` | C API、流式回调、Realtime WebSocket、发送 ring、音频输入输出分层 | 当前文本固定 `/v1/chat/completions`；SSE 有 15 KiB 固定缓冲且溢出时截断；ESP32 示例以 S3/IDF 5.5 为主，并整体引入 libwebsockets，集成面过大 |
| `espressif/esp-claw@fb7b248114bb1b12ba0fe8e03d4b59bdbec292c1` | provider 配置、TLS、错误归一化、取消思路 | OpenAI-compatible 后端非流式、完整响应缓冲、不支持 Responses |
| `openai/openai-realtime-embedded` | Realtime 会话状态、音频队列和事件命名思路 | 目标和传输重点是 OpenAI Realtime，不是当前文本 REST，也不是豆包协议 |
| `78/xiaozhi-esp32` | 音频任务、队列、打断与播放编排 | 使用自有服务端协议，不能作为 OpenAI/方舟协议实现 |
| Espressif `esp_websocket_client` | 未来 P4 上的 IDF 原生 WebSocket transport 首选候选 | 加入前必须在本项目 IDF 6.1/P4 上锁版本并做生命周期、重连和内存探针 |

允许参考 Apache-2.0/MIT 实现，但若复制了非平凡代码，必须保留原许可证头、在组件 `NOTICE.md` 记录来源文件和固定提交。优先借鉴结构和测试用例，不直接搬运整套网络栈。

## 3. 当前原型必须替换的部分

当前 `components/otool_llm_sdk` 只是可验证链路的原型，以下行为不能进入正式实现：

- 传输层写死火山方舟 Chat Completions endpoint；
- 通过字符串搜索解析 JSON 字段；
- 把网络 chunk 当成完整 SSE 行或完整 JSON；
- 只有 `CONNECTED/DELTA/DONE/ERROR`，无法表达 incomplete、cancelled、usage 和 provider error；
- 没有 request handle，无法从另一任务可靠取消；
- 未接入 `esp_crt_bundle_attach`；
- 缓冲按需 `realloc`，没有协议级硬上限；
- 没有 HTTP content type、terminal event、断流和重复回调校验；
- API Key 由 `main/CMakeLists.txt` 编译进固件。

`main/main.cpp` 尚未调用旧 API，所以本次可以直接调整公开 API，无需为未使用的 `otool_llm_chat_stream()` 保持兼容。若实现期间需要过渡 wrapper，必须标为 deprecated，并在应用切换后删除。

## 4. 目标分层

```text
应用 worker / UI queue / 对话状态 / 音频 HAL（组件外）
                         │
              public C API + opaque handles
                         │
       ┌─────────────────┴─────────────────┐
       │                                   │
 text request engine                 voice session engine（后续）
       │                                   │
 protocol adapter                    voice protocol adapter
 Responses SSE / Chat SSE            Volc TTS / ASR / S2S / OpenAI Realtime
       │                                   │
 provider preset                     voice credential + capability
 OpenAI / Ark / Custom                Volc speech / Custom
       │                                   │
 HTTP stream transport               WebSocket transport
 esp_http_client + TLS                esp_websocket_client 候选 + TLS
       └─────────────────┬─────────────────┘
           公共错误、取消、日志脱敏、内存上限
```

关键约束：

- provider 只提供默认 base URL、path、鉴权和 capability；
- protocol adapter 只负责 JSON 构建和事件映射；
- transport 不认识 `choices`、`response.output_text.delta` 或豆包业务字段；
- UI、Live2D、会话历史和整段文本拼接不进入 SDK；
- HTTP 与 WebSocket 不强行抽象为一个读写 vtable，避免最低公分母设计。它们只共享错误、TLS 配置、凭证脱敏和取消约定。

## 5. 文本 MVP 的公开 API 契约

### 5.1 头文件拆分

第一阶段公开：

- `include/otool_llm_sdk.h`：版本、公共错误、client/request handle；
- `include/otool_llm_text.h`：文本消息、请求、流式事件和执行函数。

语音阶段再新增：

- `include/otool_llm_voice.h`：音频格式、voice session 和 voice event。

公开头不能暴露 `cJSON`、`esp_http_client_handle_t` 或 WebSocket 实现类型。

### 5.2 建议 API 形状

以下是实现契约，不要求逐字照抄命名，但生命周期和所有权不得改变：

```c
typedef struct otool_llm_client *otool_llm_client_handle_t;
typedef struct otool_llm_request *otool_llm_request_handle_t;

typedef enum {
    OTOOL_LLM_PROVIDER_OPENAI,
    OTOOL_LLM_PROVIDER_VOLCENGINE_ARK,
    OTOOL_LLM_PROVIDER_CUSTOM,
} otool_llm_provider_t;

typedef enum {
    OTOOL_LLM_PROTOCOL_AUTO,
    OTOOL_LLM_PROTOCOL_RESPONSES_SSE,
    OTOOL_LLM_PROTOCOL_CHAT_COMPLETIONS_SSE,
} otool_llm_protocol_t;

typedef struct {
    size_t struct_size;
    otool_llm_provider_t provider;
    otool_llm_protocol_t protocol;
    const char *base_url;       /* NULL = provider default */
    const char *responses_path; /* NULL = /responses */
    const char *chat_path;      /* NULL = /chat/completions */
    const char *api_key;        /* create 时深拷贝；不得来自编译宏 */
    int connect_timeout_ms;
    int read_timeout_ms;
} otool_llm_client_config_t;

typedef enum {
    OTOOL_LLM_ROLE_DEVELOPER,
    OTOOL_LLM_ROLE_SYSTEM,
    OTOOL_LLM_ROLE_USER,
    OTOOL_LLM_ROLE_ASSISTANT,
} otool_llm_role_t;

typedef struct {
    otool_llm_role_t role;
    const char *text;
} otool_llm_text_message_t;

typedef struct {
    size_t struct_size;
    const char *model;
    const char *instructions;
    const otool_llm_text_message_t *messages;
    size_t message_count;
    const char *previous_response_id; /* 仅 Responses */
    int max_output_tokens;            /* 0 = 未设置 */
    float temperature;
    bool temperature_is_set;
    bool store;                       /* 默认 false */
} otool_llm_text_request_t;

esp_err_t otool_llm_client_create(
    const otool_llm_client_config_t *config,
    otool_llm_client_handle_t *out_client);
void otool_llm_client_destroy(otool_llm_client_handle_t client);

esp_err_t otool_llm_request_create(
    otool_llm_client_handle_t client,
    const otool_llm_text_request_t *request,
    otool_llm_request_handle_t *out_request);
esp_err_t otool_llm_request_execute_stream(
    otool_llm_request_handle_t request,
    otool_llm_text_event_cb_t callback,
    void *user_ctx);
esp_err_t otool_llm_request_cancel(otool_llm_request_handle_t request);
void otool_llm_request_destroy(otool_llm_request_handle_t request);
```

必须使用 `struct_size` 做版本检查，新增字段只能尾部追加。`request_create()` 深拷贝 model、instructions、messages 和 previous response ID，调用方在创建成功后可以释放原始字符串。

### 5.3 流式事件

至少提供：

- `RESPONSE_STARTED`：拿到 response/request ID 和 model 时触发；
- `TEXT_DELTA`：只传本次 UTF-8 增量；
- `TEXT_DONE`：文本内容结束，不重复传完整文本；
- `USAGE`：输入、输出、总 token，未知值用显式 unavailable 表达；
- `COMPLETED`：唯一正常终止事件；
- `INCOMPLETE`：达到 token/内容限制等非正常完整结束；
- `CANCELLED`：本地取消完成；
- `ERROR`：transport、TLS、HTTP、provider、JSON 或 protocol 错误。

事件中的字符串和二进制 span 只在 callback 返回前有效。SDK 不累计整段回答；需要完整回答的调用方自行拼接。

callback 返回值应至少包含 `CONTINUE` 和 `CANCEL`。返回 `CANCEL` 与跨任务调用 `otool_llm_request_cancel()` 等价。

每个 request **恰好产生一个 terminal event**：`COMPLETED`、`INCOMPLETE`、`CANCELLED`、`ERROR` 四选一。断开连接、HTTP finish 和 provider terminal event 不能各自重复派发完成事件。

### 5.4 线程、取消与销毁规则

- `request_execute_stream()` 阻塞，并在调用它的任务上下文中执行 callback；
- SDK 不创建文本 worker task；应用负责在非 LVGL 任务中调用；
- callback 中不得直接调用 LVGL，应用需要复制 delta 后投递到 UI queue；
- `request_cancel()` 可以由另一任务调用，必须幂等；
- 取消标志和 active HTTP handle 受锁保护；只有执行任务负责 cleanup；
- `request_destroy()` 只能在 execute 返回后调用，否则记录错误并拒绝销毁；
- MVP 每个 client 只允许一个 in-flight request，并发调用返回 `ESP_ERR_INVALID_STATE`；不要用未验证的多并发增加 TLS 峰值。

## 6. Provider 与协议 adapter

### 6.1 Provider preset

内部 `provider_config` 至少包含：

- 默认 base URL；
- Responses/Chat path；
- auth 类型和 header 构造函数；
- 默认 protocol；
- capability bitmask；
- provider error 和 request/log ID 提取函数。

`PROTOCOL_AUTO` 只按 provider 的静态默认值解析，**禁止发送探测请求**。OpenAI 和 Ark 默认 Responses；Custom 必须显式选协议。

请求含 provider 不支持的字段时必须返回明确错误，禁止静默丢字段。例如 Chat adapter 收到 `previous_response_id` 时应拒绝，而不是悄悄忽略。

### 6.2 Responses adapter

请求体第一阶段支持：

- `model`；
- `input`：纯文本 message 数组；
- `instructions`；
- `stream: true`；
- `store`，设备侧默认 `false`；
- `previous_response_id`；
- `max_output_tokens`；
- `temperature`（仅在显式设置时发送）。

核心事件映射：

| SSE/JSON type | SDK 事件 | 处理要求 |
| --- | --- | --- |
| `response.created` | `RESPONSE_STARTED` | 保存 id/model，不把完整对象长期保留 |
| `response.output_text.delta` | `TEXT_DELTA` | 读取 `delta`，允许空 delta，校验字符串类型 |
| `response.output_text.done` | `TEXT_DONE` | 不重新发送完整 `text`，避免重复 UI 输出 |
| `response.completed` | `USAGE`（若有）+ `COMPLETED` | 正常唯一终止 |
| `response.incomplete` | `USAGE`（若有）+ `INCOMPLETE` | 传递原因 |
| `response.failed` 或 `error` | `ERROR` | 解析 code/message/request ID |
| 未知事件 | 忽略并计数/DEBUG 记录事件名 | 不因新增字段崩溃 |

工具调用、reasoning delta、图片和音频不是文本 MVP 的验收项。解析器必须容忍这些未知事件；后续能力要通过新增事件类型扩展，不能把原始 JSON 暴露为默认业务 API。

### 6.3 Chat Completions adapter

请求使用 `messages`、`stream: true`，并在 provider 支持时加入 `stream_options.include_usage`。max token 字段由 provider capability 决定，不能在 transport 中写死。

核心映射：

- `choices[*].delta.content` → `TEXT_DELTA`；
- `finish_reason` 非空 → 记录完成原因；
- usage chunk → `USAGE`；
- `data: [DONE]` → 在前面没有错误时 `COMPLETED`；
- HTTP 正常关闭但从未收到 `[DONE]` → `ERROR(PROTOCOL_EOF)`，不能当正常完成。

第一阶段只接受单 choice（index 0）。若服务返回多个 choice，明确报 unsupported，不能交错输出。

## 7. HTTP、TLS 与 SSE 状态机

### 7.1 HTTP transport

必须做到：

- 使用 `esp_http_client`；
- HTTPS 默认强制启用；
- `.crt_bundle_attach = esp_crt_bundle_attach`；
- 200–299 且 `Content-Type` 为 `text/event-stream` 才进入 SSE adapter；
- 非 2xx 响应读取有界错误体，再交给 provider error parser；
- 收集常见 request ID/log ID header，但不记录 Authorization；
- 支持 connect/read timeout；
- 取消时调用 IDF 支持的 cancel API，执行任务统一清理；
- MVP 每次请求创建独立 HTTP handle；连接复用以后在 client 内局部实现，不采用 ESP-Claw 的全局 linker wrapping。

MVP 不做自动重试。POST 已经发出但响应丢失时，服务端可能已经计费或生成内容；自动重试会产生重复响应。SDK只上报 `Retry-After` 等信息，由应用显式决定是否重新发起新 request。

### 7.2 SSE 解析器必须是独立、可测试的字节状态机

网络回调的每个 chunk 都可能落在任意位置，包括 UTF-8 多字节中间、字段名中间、`\n` 中间或 JSON 字符串中间。解析流程必须是：

1. 输入 `(uint8_t *, length)`，不依赖 NUL 结尾；
2. 识别 CRLF、LF 和单 CR 行结尾；
3. 按第一个 `:` 分隔 field/value，只去掉 value 起始处至多一个空格；
4. 忽略 `:` comment；
5. 支持 `event`、`data`、`id`、`retry`；
6. 多个 `data:` 行用 `\n` 合并；
7. 空行才派发一个完整 event；
8. 派发后清除 data/event，保留规范要求的 last event ID；
9. 超过上限返回 `EVENT_TOO_LARGE`，禁止截断后继续解析；
10. EOF 时若有未派发半个事件，报告 protocol EOF，不伪造完成。

SSE parser 只产出 `{event_name, data, id}`，`[DONE]` 和 JSON 字段含义由协议 adapter 处理。

## 8. JSON、内存与安全约束

### 8.1 JSON

- ESP-IDF 6 使用 managed dependency `espressif/cjson`；
- 请求通过 cJSON 构建，禁止字符串拼 JSON；
- 使用 `cJSON_PrintPreallocated()` 写入有上限的请求 buffer；
- 每个完整 SSE data event 单独 parse，callback 返回后立即释放树；
- 所有字段先检查 JSON type，再读取；
- 未知字段忽略；缺少必需字段返回 provider protocol error；
- 不全局替换 cJSON allocator hooks，避免影响其他组件和线程。

### 8.2 初始 Kconfig 预算

下列值是首轮安全默认值，真机测量后可调整，但实现必须始终有硬上限：

| Kconfig | 默认值 | 说明 |
| --- | ---: | --- |
| `OTOOL_LLM_MAX_REQUEST_BYTES` | 32768 | 序列化 JSON 请求上限 |
| `OTOOL_LLM_MAX_SSE_EVENT_BYTES` | 16384 | 单个合并后 SSE data 上限 |
| `OTOOL_LLM_MAX_ERROR_BODY_BYTES` | 4096 | 非 2xx 错误体上限 |
| `OTOOL_LLM_HTTP_RX_BUFFER_BYTES` | 2048 | HTTP 接收 chunk buffer |
| `OTOOL_LLM_CONNECT_TIMEOUT_MS` | 15000 | 默认连接超时 |
| `OTOOL_LLM_READ_TIMEOUT_MS` | 60000 | 默认流读取超时 |
| `OTOOL_LLM_ALLOW_INSECURE_HTTP` | `n` | 仅本地测试可打开 |

如果 buffer 不足，返回明确错误；禁止 silent truncation。测试报告必须记录 TLS 握手时最小 free heap、largest free block、请求期间峰值和连续 100 次请求后的净差值。

### 8.3 凭证

在任何功能开发前先完成：

1. 撤销/轮换当前出现在 `main/CMakeLists.txt` 中的方舟 Key；
2. 删除 CMake compile definition 和任何真实密钥；
3. 示例只使用占位符；
4. 原型从运行时配置/NVS 读取；量产优先使用自有网关和设备短期凭证；
5. client 深拷贝 secret，destroy 时用不可被优化掉的 secure zero 清理；
6. 错误、DEBUG、崩溃日志都不输出 header、Key、完整请求体或用户完整内容。

即使 Key 由 CI 环境变量注入，只要编译到固件仍可被提取，因此不能把“未提交 Git”当成量产安全方案。

## 9. 目标目录

```text
components/otool_llm_sdk/
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── README.md
├── NOTICE.md                         # 仅在复制第三方实现后需要
├── include/
│   ├── otool_llm_sdk.h
│   └── otool_llm_text.h
├── private_include/
│   ├── otool_llm_internal.h
│   ├── otool_llm_provider.h
│   ├── otool_llm_protocol.h
│   └── otool_llm_transport.h
├── src/
│   ├── core/
│   │   ├── client.c
│   │   ├── request.c
│   │   ├── error.c
│   │   └── secure_zero.c
│   ├── providers/
│   │   ├── provider_openai.c
│   │   ├── provider_ark.c
│   │   └── provider_custom.c
│   ├── protocols/
│   │   ├── responses_sse.c
│   │   └── chat_completions_sse.c
│   └── transports/
│       ├── http_stream.c
│       └── sse_parser.c
├── test_apps/
│   └── parser_and_adapters/          # 最小 ESP-IDF Unity app
└── test_fixtures/
    ├── openai_responses/
    ├── ark_responses/
    ├── openai_chat/
    └── ark_chat/
```

语音阶段再添加 `src/voice/`、`src/transports/websocket.c`、`include/otool_llm_voice.h` 和独立 fixture；不要在文本 MVP 先放空壳文件。

## 10. 依赖策略

文本 MVP 的 `idf_component.yml`：

```yaml
dependencies:
  idf:
    version: ">=6.1.0"
  espressif/cjson: "^1.7.19~2"
```

`CMakeLists.txt` 只注册实际源文件，私有依赖使用 `PRIV_REQUIRES esp_http_client mbedtls cjson`。

语音阶段不要提前引入 libwebsockets。先对 Espressif managed `esp_websocket_client` 做 P4/IDF 6.1 build/runtime probe，再锁定精确版本；探针必须覆盖连接、双向二进制帧、fragment、跨任务发送、stop/destroy、断网和 100 次重连后的 heap。

## 11. 后续 agent 的实施任务包

每个任务包都必须独立编译或测试通过后再进入下一包，禁止在一次大改中同时引入传输、协议、UI 和语音。

### WP0：安全清理与基线

修改：

- `main/CMakeLists.txt`：删除真实 Key 和 compile definition；
- 记录当前固件 build 结果、size、free heap 基线；
- 确认应用层已有 `NETWORK_READY` 和时间同步前置条件，没有则只记录调用契约，不让 SDK接管网络初始化。

验收：仓库 `rg` 找不到真实 secret；旧 Key 已在控制台轮换；固件仍可编译。

### WP1：组件骨架与公开 API

修改：

- 拆分公开头、opaque handles、client/request 生命周期；
- provider preset 表和 protocol vtable；
- Kconfig、manifest、README；
- 删除单文件旧 API 或仅保留明确 deprecated wrapper。

验收：OpenAI/Ark/Custom 三种 config 的纯参数测试通过；无网络时创建/销毁无泄漏；非法状态有确定返回值。

### WP2：SSE parser 先行

实现纯字节状态机，不引用 `esp_http_client` 或 provider 代码。先写测试再接网络。

必测 fixture：

- 每个可能字节位置分片；
- CRLF/LF/CR；
- 多行 data；
- comment、event、id、retry；
- emoji UTF-8 跨 chunk；
- 一次 chunk 多事件；
- 空 data、未知字段；
- 恰好到上限和超过上限；
- EOF 半事件。

验收：同一 fixture 对所有分片组合产生相同事件序列；ASan host test 若环境可用则必须运行，至少 ESP-IDF Unity 测试必须通过。

### WP3：Responses 与 Chat adapter

实现 cJSON 请求构建和脱敏 fixture 解析，不连公网。

验收：

- OpenAI Responses 与 Ark Responses fixture 都能输出正确 delta/terminal；
- Chat `[DONE]`、usage、finish reason 正确；
- unknown event 不崩溃；
- 类型错误、缺字段、重复 terminal 都变成单一 `ERROR`；
- adapter 单测中不出现 provider endpoint 或 HTTP API。

### WP4：HTTP/TLS/cancel

接入 `esp_http_client`、certificate bundle、status/content-type/header/error body 和取消。

先用本地可控 SSE server 做：任意 chunk、慢响应、提前断开、401、429、500、错误 JSON、取消。然后才做公网 smoke test。

验收：取消不会死锁或 double free；断流不是成功；TLS 校验开启；Authorization 不进日志；超长事件返回明确错误。

### WP5：双 provider live smoke test

使用运行时临时凭证，默认各做一条最短文本请求：

- OpenAI Responses；
- Ark Responses；
- Ark Chat 后备协议。

对照 OpenAI 官方 SDK 与 `volcenginesdkarkruntime` 的脱敏请求/事件 fixture。live test 必须 opt-in，CI 无凭证时跳过而不是失败。

验收：中文、emoji、长于一个网络 chunk 的输出无丢字/重复；每次恰好一个 terminal；provider request/log ID 可用于排障但无凭证泄漏。

### WP6：应用接入

应用层新建 LLM worker 和 UI queue：

- worker 创建 request 并阻塞执行；
- callback 复制 delta 到有界消息后投递 UI；
- UI 线程逐段更新文本/Live2D 状态；
- 新请求可取消旧请求；
- 网络断开、页面退出和设备关机走统一取消/销毁。

验收：LLM 生成期间 LVGL 帧率和触摸不被阻塞；页面退出无悬挂 callback；队列满有明确策略，不无限分配。

### WP7：可靠性与资源报告

执行 100 次短请求、断网恢复、连续取消、429/5xx、OOM 注入和超长事件。报告：

- 首 delta 延迟；
- 每次总时延；
- task stack high-water mark；
- minimum free heap 和 largest block；
- 100 次后 heap 净差；
- 每种失败的 terminal event 与返回码。

验收：无增长性泄漏、无 watchdog、无 UI 阻塞、无 secret 日志。达不到预算时先修资源问题，不直接开始语音。

### WP8：语音协议选型 ADR（文本稳定后才开始）

由产品明确选择：TTS、级联 ASR→LLM→TTS、或端到端 S2S。agent 写一份单独 ADR，至少包含：

- 官方 endpoint 与协议版本；
- 控制台需开通的产品；
- 凭证/header；
- 输入输出 codec、sample rate、channel、bit depth；
- 是否 gzip/base64/二进制帧；
- session start/end/interrupt 事件；
- P4 实测首包延迟和 heap；
- WebSocket component 固定版本。

ADR 评审通过前，不实现所谓“通用豆包语音 adapter”。

## 12. 文本测试矩阵

| 维度 | 用例 |
| --- | --- |
| SSE framing | 任意分片、CRLF/LF/CR、多事件、multi-data、comment、半事件 EOF |
| JSON | 中文、emoji、转义、空 delta、字段乱序、未知字段、错误类型、无效 JSON |
| Responses | created、text delta/done、completed、incomplete、failed、error、usage |
| Chat | role chunk、content delta、空 delta、finish reason、usage、`[DONE]` |
| HTTP | 200、204、错误 content type、400/401/403/408/429/5xx、Retry-After |
| TLS/network | DNS、证书失败、connect timeout、read timeout、Wi-Fi 中断、提前 EOF |
| lifecycle | 正常、callback cancel、跨任务 cancel、重复 cancel、destroy 时序、连续请求 |
| limits | request/event/error body 边界、OOM、queue 满、超长 provider message |
| security | Key 不入库/日志/错误体；endpoint 默认 HTTPS；错误日志脱敏 |
| resources | 峰值 heap、largest block、stack high-water、100 次净差 |

fixture 分两类：

1. 手工最小 fixture：覆盖每个边界和错误；
2. 官方 SDK录制 fixture：由主机脚本调用官方 SDK，保存前删除 Key、用户敏感文本、完整 header，仅保留协议结构和虚构内容。

## 13. 语音扩展时必须保留的接口缝

文本 MVP 不实现语音，但内部命名和公共错误不能阻断以下设计：

```c
typedef struct otool_llm_voice_session *otool_llm_voice_session_handle_t;

typedef struct {
    otool_llm_audio_codec_t codec;
    uint32_t sample_rate_hz;
    uint8_t channels;
    uint8_t bits_per_sample;
} otool_llm_audio_format_t;

esp_err_t otool_llm_voice_session_open(...);
esp_err_t otool_llm_voice_session_write_audio(...);
esp_err_t otool_llm_voice_session_send_text(...);
esp_err_t otool_llm_voice_session_interrupt(...);
esp_err_t otool_llm_voice_session_close(...);
```

未来 voice event 至少区分：session ready、input speech started/stopped、transcript delta/final、audio format、audio chunk、response done、interrupted、error。

实现约束：

- WebSocket text/binary frame 必须处理 fragment 和 FIN；
- 发送队列有界，音频不能每帧无上限 malloc；
- output audio span 只在 callback 内有效，应用立即复制到播放器 ring；
- 网络 callback 不直接写 I2S，也不长时间阻塞；
- 不自动重放 audio session；断线后由上层决定是否新建 session；
- 豆包二进制协议的大小端、sequence、压缩和 terminal flag 在独立 codec 测试；
- 语音凭证结构支持多 header，不复用 Ark Bearer config；
- TTS、ASR、S2S 是三个 adapter，可共享 WebSocket transport，但不共享业务状态机。

## 14. Definition of Done

文本 SDK 只有同时满足以下条件才算完成：

- ESP32-P4 / 当前 ESP-IDF 6.1 工程编译链接通过；
- OpenAI Responses、Ark Responses、Ark Chat 三条流式 smoke test 通过；
- SSE 所有分片与上限测试通过；
- HTTPS 证书校验开启；
- 跨任务取消稳定且每次只有一个 terminal event；
- 无手写 JSON 字符串拼接和字符串搜索 JSON；
- 无未设上限的 response/event buffer；
- 无真实 Key、Authorization 或完整敏感请求日志；
- 应用 UI 不被网络调用阻塞；
- 资源报告和失败矩阵进入 `docs`；
- README 给出运行时凭证示例、阻塞/回调线程说明和取消示例。

以下内容不属于文本 MVP：工具调用、图片、多 choice、自动重试、连接池、HTTP/2、Realtime voice、音频 HAL。不得以它们未完成为由扩大第一阶段范围。

## 15. 给执行 agent 的工作规则

1. 先读本文和现有 `components/otool_llm_sdk`，不要从旧单文件继续堆 `if (provider)`。
2. 每完成一个 WP 更新本文对应状态或另建实施记录，并附真实命令与测试结果。
3. 不修改无关 Live2D/Cubism 代码。
4. 不提交下载的参考仓库；参考源码放系统临时目录，记录固定 commit 后删除即可。
5. 遇到方舟事件与 OpenAI 不一致时，新增 Ark adapter 差异测试，不用宽松字符串搜索绕过。
6. 遇到协议文档和现场响应冲突时，保存脱敏原始 fixture，记录日期、model 和 request/log ID，再做兼容判断。
7. 任何为“先跑通”而关闭 TLS、写死 Key、无限扩容或在 UI 线程请求网络的改动都不接受。

## 16. 资料基线

- [OpenAI Responses create reference](https://developers.openai.com/api/reference/typescript/resources/beta/subresources/responses/methods/create)：`POST /responses`、SSE 与事件结构。
- [OpenAI Realtime model/API](https://developers.openai.com/api/docs/models/gpt-realtime)：未来语音会话与 WebSocket 思路参考，不是文本 MVP 依赖。
- [火山方舟产品简介与 Responses 示例](https://www.volcengine.com/docs/82379/1795150)：方舟 `/api/v3/responses` 及 OpenAI SDK 兼容调用。
- [火山方舟 Responses 工具调用](https://www.volcengine.com/docs/82379/1958524?lang=zh)：Responses 对象和多轮结构参考。
- [豆包端到端实时语音大模型](https://www.volcengine.com/docs/6561/1594360?lang=zh)：端到端 S2S 产品边界。
- [豆包语音 V3 接口选择说明](https://www.volcengine.com/docs/6561/2228192?lang=zh)：TTS WebSocket、HTTP chunked 与 SSE 路线。
- [豆包双向流式语音合成 WebSocket](https://www.volcengine.com/docs/6561/2532486?lang=zh)：未来 TTS adapter 的官方协议入口。
- [volcengine/onesdk](https://github.com/volcengine/onesdk)：官方边缘 C SDK参考，审计快照 `657fceccab227bd860c984cdf37aeae3e68c5b4a`。
- [espressif/esp-claw](https://github.com/espressif/esp-claw)：provider、TLS 和取消参考，审计快照 `fb7b248114bb1b12ba0fe8e03d4b59bdbec292c1`。
- [Espressif esp_websocket_client](https://github.com/espressif/esp-protocols/tree/master/components/esp_websocket_client)：未来 P4 WebSocket transport 候选。
- [Espressif cJSON component](https://components.espressif.com/components/espressif/cjson/versions/1.7.19~2/readme)：ESP-IDF 6 managed JSON dependency。
- [WHATWG Server-Sent Events processing model](https://html.spec.whatwg.org/multipage/server-sent-events.html)：SSE 字节与字段状态机基准。
