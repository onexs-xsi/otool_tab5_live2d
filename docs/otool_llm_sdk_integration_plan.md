# `otool_llm_sdk` 集成 OpenAI 与火山方舟方案

> 状态：方案评审稿  
> 调研日期：2026-08-21  
> 目标平台：ESP32-P4 / ESP-IDF 6.1 系列  
> 范围：分析与实施计划，不包含本轮代码实现

> **最终决策更新（2026-08-22）：** 产品需要逐增量流式输出，并要为后续豆包语音预留 WebSocket/二进制协议能力，因此保留并重构自研 `otool_llm_sdk`，不再由 ESP-Claw 直接替换。后续 agent 应以 [`otool_llm_sdk_streaming_voice_implementation_plan.md`](./otool_llm_sdk_streaming_voice_implementation_plan.md) 为权威执行计划；本文保留为背景分析。

## 1. 结论

不能把 OpenAI 或火山方舟现有的“官方 SDK”直接链接进当前固件：

- OpenAI 当前列出的官方应用 SDK 是 JavaScript、Python、.NET、Java、Go、Ruby，没有 C/C++；官方同时明确允许应用使用自己的 HTTP 客户端。
- 火山方舟当前公开的模型调用方式覆盖 Python、Go、Java、Curl 和 OpenAI SDK 兼容调用，没有适合 ESP-IDF 的 C/C++ SDK。
- 用户找到的 `volcengine-python-sdk` 是官方仓库，其中 `volcenginesdkarkruntime`/`volcengine-python-sdk[ark]` 正是方舟推理的 Python SDK，但 Python、`httpx`、线程和其依赖树无法成为 ESP32-P4 固件依赖。

因此建议采用两层定义的“官方 SDK/API 为源头”：

1. **设备端运行时**：使用 ESP-IDF 官方 `esp_http_client`，严格按照 OpenAI 与火山方舟官方 REST/SSE 文档实现小型 C 适配器。
2. **主机端一致性基准**：用两家的官方 SDK 产生基准请求、录制脱敏响应并做协议回归；若产品要安全量产，则进一步把官方 SDK 放到网关，设备只访问自有网关。

这不是把第三方 C++ 封装冒充官方 SDK。固件中的事实来源仍然是官方 API；官方 SDK 用作协议基准或网关运行时。

当前仓库适合先完成“设备直连 MVP”，但**正式分发固件时强烈建议使用网关**，否则长期 API Key 必须存在设备上，很难真正保密。

## 2. 官方支持现状

| 方案 | OpenAI | 火山方舟 | 能否直接进入 ESP-IDF 固件 | 建议用途 |
| --- | --- | --- | --- | --- |
| 官方 Python SDK | 支持 | 支持，`volcengine-python-sdk[ark]` | 否 | 主机基准、测试、网关 |
| 官方 Go SDK | 支持 | 支持 | 否，ESP-IDF 目标不适合 Go 运行时 | 单文件网关服务 |
| 官方 Java SDK | 支持 | 支持 | 否 | Java 网关 |
| 官方 C/C++ SDK | 未提供 | 未提供 | 不适用 | 不应等待或假设存在 |
| OpenAI SDK 兼容调用火山方舟 | 主机侧 OpenAI SDK | 方舟官方文档给出兼容方式 | 否，OpenAI 也没有官方 C++ SDK | 主机验证、网关 |
| 官方 REST API + `esp_http_client` | 支持 | 支持 | **是** | 当前设备端推荐方案 |
| 非官方 `openai-cpp` 一类库 | 非官方 | 兼容性不确定 | 理论可移植但风险高 | 不采用 |
| 从 OpenAPI 自动生成 C++ 客户端 | 生成物，不是官方 C++ SDK | 同理 | 依赖/体积/异常与动态分配通常不适合 MCU | 不采用 |

对本项目而言，用户给出的火山 SDK 仓库已经是正确的**主机端官方来源**。没有一个更好的官方 C++ 方舟 SDK可以替换它；更合适的改变是区分“主机官方 SDK”和“设备轻量协议客户端”。

## 3. 当前仓库评估

### 3.1 已有基础

`components/otool_llm_sdk` 已经具备一个最小可运行形态：

- C API，便于 C/C++ 调用；
- `esp_http_client` HTTPS POST；
- Chat Completions JSON 请求；
- 按行接收 SSE，并通过回调输出文本增量；
- 阻塞调用语义明确，可放到独立 FreeRTOS 任务。

这些代码可以作为传输层原型保留，但不适合直接扩展成双官方提供商版本。

### 3.2 必须先处理的问题

#### P0：凭证安全

`main/CMakeLists.txt` 当前包含一个看起来可用的方舟 API Key，并通过编译宏写入固件。应视为已经暴露：

1. 立即在方舟控制台撤销/轮换该 Key；
2. 从 CMake、源码、日志和提交历史中移除真实 Key；
3. 原型阶段改为运行时配置，至少通过串口配网/配置流程写入加密 NVS；
4. 量产阶段由网关保存提供商 Key，设备仅持有可撤销的设备凭证或短期令牌。

即使从环境变量在编译时注入，Key 最终仍会进入固件镜像，因此只解决“不要提交到 Git”，不能解决设备侧提取问题。

#### P0：网络前置条件尚未闭环

当前 `main/main.cpp` 只初始化 NVS、硬件、LVGL，没有建立 Wi-Fi/IP、DNS 与时间同步，也没有等待网络就绪。ESP32-P4 使用远端 Wi-Fi 方案时，LLM 组件不应自行抢占网络初始化职责，但调用前必须有明确的 `NETWORK_READY` 前置状态。

#### P0：HTTPS 服务端校验未接入

工程的 `sdkconfig` 已启用证书包，但现有 `esp_http_client_config_t` 没有设置 `crt_bundle_attach` 或 `cert_pem`。Espressif 官方文档要求 HTTPS 服务端校验通过这两种方式之一配置。建议引入 `esp_crt_bundle.h` 并设置：

```c
.crt_bundle_attach = esp_crt_bundle_attach,
```

禁止以跳过证书校验作为正式修复。

#### P1：JSON/SSE 解析过于脆弱

当前实现用字符串搜索提取 `content`，存在以下问题：

- 不是结构化 JSON 解析，可能匹配错误层级；
- 固定 1024 字节文本缓冲会截断较大事件；
- `\uXXXX` 没有完整处理 UTF-16 代理对；
- 只处理单行 `data:`，没有按 SSE 空行组装完整事件；
- OpenAI Responses 流的文本字段是事件对象中的 `delta`，不是 Chat Completions 的 `choices[].delta.content`；
- 连接结束时即使没有收到协议终止事件，也可能被报告为成功。

ESP-IDF 6 已移除内置 `json` 组件，应通过组件管理器添加 `espressif/cjson`，不要继续扩大手写 JSON 解析器。

#### P1：当前公共 API 绑定单一协议

现有结构只有一个全局 endpoint、一个 `model` 和一条 user message，并将认证固定为 Bearer。还缺少：

- provider 与协议选择；
- 多轮输入或 `previous_response_id`；
- Responses API 的 `instructions`、`input`、`max_output_tokens`；
- usage、response id、finish/failure/incomplete 状态；
- HTTP 状态、提供商错误码、request id；
- 取消正在进行的请求；
- 对 429/5xx/超时的受控重试策略。

## 4. 推荐架构

```text
UI / Live2D 状态机
        |
        | FreeRTOS queue（禁止网络回调直接操作 LVGL）
        v
LLM worker task
        |
        v
otool_llm 公共 C API
        |
        +--> OpenAI Responses adapter ------+
        |                                    |
        +--> Volcengine Responses adapter ---+--> JSON + SSE --> esp_http_client --> TLS
        |
        +--> Volcengine Chat adapter（仅迁移期兼容，后续可删）
```

### 4.1 公共层

公共层只表达产品真正需要的能力，不直接暴露任一家 SDK 的完整对象模型。建议的最小概念如下：

```c
typedef enum {
    OTOOL_LLM_PROVIDER_OPENAI,
    OTOOL_LLM_PROVIDER_VOLCENGINE,
} otool_llm_provider_t;

typedef struct {
    otool_llm_provider_t provider;
    const char *api_key;
    const char *base_url;   /* NULL 使用 provider 默认值 */
    const char *model;
    int timeout_ms;
} otool_llm_client_config_t;

typedef struct {
    const char *instructions;
    const char *input_text;
    const char *previous_response_id;
    int max_output_tokens;
    float temperature;
    bool store;
} otool_llm_request_t;
```

事件至少应包含：

- `CONNECTED`；
- `RESPONSE_STARTED`，携带 response id；
- `TEXT_DELTA`；
- `USAGE`；
- `COMPLETED`；
- `INCOMPLETE`；
- `CANCELLED`；
- `ERROR`，携带 transport error、HTTP status、provider code、provider message、request id。

MVP 先只承诺文本输入/文本流输出。工具调用、图片、音频、文件上传与结构化输出后续按实际产品需求增量加入，避免为了“仿完整 SDK”把固件做成不可维护的大客户端。

### 4.2 Provider 适配层

每个 provider 独立负责：

- 默认 base URL 与 path；
- 请求 JSON 构建；
- provider 特有参数映射；
- SSE 事件类型与 JSON 字段解析；
- 错误体解析；
- API 能力声明。

不要仅因为火山方舟支持 OpenAI SDK 兼容调用，就让两个 provider 共用一份无差别解析器。兼容层可能只覆盖部分字段，版本演进速度也可能不同；共享 HTTP、SSE 和 JSON 基础设施，保留两个协议适配器更稳妥。

### 4.3 传输与 SSE 层

建议把以下逻辑从 provider 中抽离：

- `esp_http_client` 生命周期与 HTTPS 配置；
- Authorization header 注入与日志脱敏；
- HTTP 状态、响应 header、超时与底层 TLS 错误收集；
- SSE 任意分片拼接；
- `event:`、多行 `data:`、注释、CRLF、空行派发；
- 单个 SSE event 最大字节数限制；
- 用户取消与 deadline 检查。

调用可以继续是阻塞式，但必须在独立 worker task 中执行。ESP-IDF 已提供 `esp_http_client_cancel_request()`，公共 API 可以基于它增加取消能力。

### 4.4 默认协议选择

新实现以 **Responses API** 为主线：

| Provider | 默认 endpoint | 主要文本增量 | 正常终止 |
| --- | --- | --- | --- |
| OpenAI | `https://api.openai.com/v1/responses` | `response.output_text.delta` 的 `delta` | `response.completed` |
| 火山方舟 | `https://ark.cn-beijing.volces.com/api/v3/responses` | 按方舟官方 Responses 流事件验证后映射 | 方舟完成事件 |

OpenAI 官方已建议所有新项目使用 Responses API；Chat Completions 仍受支持。火山方舟也已经公开 Responses API，并给出了 Python、Go、Java、Curl 和 OpenAI SDK 示例。因此以 Responses 为共同上层模型，比继续扩大当前 Chat Completions 专用接口更有长期价值。

为了降低迁移风险，可暂时保留当前火山 Chat Completions adapter，先通过原有功能验收网络链路，再切换默认协议。不要在同一解析函数里混合两套事件形状。

## 5. 建议目录与依赖

```text
components/otool_llm_sdk/
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── include/
│   └── otool_llm_sdk.h
├── private_include/
│   └── otool_llm_internal.h
├── src/
│   ├── otool_llm_client.c
│   ├── otool_llm_http.c
│   ├── otool_llm_sse.c
│   ├── otool_llm_json.c
│   └── providers/
│       ├── otool_llm_openai_responses.c
│       ├── otool_llm_volc_responses.c
│       └── otool_llm_volc_chat_legacy.c
└── test_apps/
    └── protocol_parser/
```

依赖建议：

- `esp_http_client`：HTTPS/SSE 传输；
- `mbedtls`/ESP x509 certificate bundle：服务端证书验证；
- `espressif/cjson: ^1.7.19`：JSON 构建与解析；
- Unity（仅测试应用）：协议解析与分片测试。

`Kconfig` 只保存非敏感默认项，例如超时、最大 SSE event 大小、是否启用某个 provider。API Key 不进入 Kconfig、CMake cache、编译宏或日志。

## 6. “官方 SDK 为源头”的落地方式

### 6.1 主机一致性工具

建议新增不进入固件的工具目录：

```text
tools/llm_conformance/
├── openai_reference.py
├── volcengine_reference.py
├── sanitize_fixture.py
└── fixtures/
    ├── openai_responses_sse/
    └── volcengine_responses_sse/
```

- `openai_reference.py` 使用 OpenAI 官方 Python SDK；
- `volcengine_reference.py` 使用 `volcengine-python-sdk[ark]`，并可额外用 OpenAI SDK兼容模式做交叉验证；
- 凭证只从环境变量读取；
- fixture 必须去除 Key、request id、用户敏感文本与不可稳定字段；
- 固件的 parser 单元测试重放这些 fixture，并覆盖所有网络分片位置。

这样每次升级 provider 协议时，都能先用官方 SDK确认事件形状，再修改设备适配器。

### 6.2 可选生产网关

当出现以下任一要求时，应转为网关模式：

- 固件会交付给不受控用户；
- 不能接受 provider Key 被提取；
- 需要 IAM/AK-SK 签名、临时凭证或自动刷新；
- 需要统一限流、配额、审计、内容策略和 provider failover；
- 需要快速跟进 SDK/API 变化而不升级所有设备固件。

网关可使用 OpenAI 官方 Go/Python SDK与火山方舟官方 Go/Python SDK，向设备暴露稳定的自有 SSE 协议。代价是增加一跳延迟和服务运维，但这是“官方 SDK 真正参与运行时”的可靠方式。

## 7. 分阶段实施计划

### 阶段 0：安全与连通性基线

交付物：

- 轮换当前硬编码 Key，并从构建文件移除；
- 明确开发期凭证注入与量产凭证架构；
- 应用层完成 Wi-Fi/IP/DNS/时间就绪状态；
- 使用证书包完成 OpenAI 与方舟各一次 HTTPS 无业务或最小请求 smoke test；
- 记录失败时的 TLS error、HTTP status 和可脱敏 request id。

验收：仓库和固件日志中没有真实 Key；关闭服务端证书校验的配置不存在；断网、DNS 失败、证书失败能被区分。

### 阶段 1：公共 API 与基础设施重构

交付物：

- provider-neutral client/request/event API；
- 独立 HTTP transport、SSE parser、JSON helper；
- 引入 `espressif/cjson`；
- event 大小上限、超时、取消；
- worker task 与 UI queue 集成约束。

验收：SSE 在每个字节边界分片都能得到相同结果；恶意超长 event 返回明确错误且不无限扩容；没有协议终止事件时不报告成功。

### 阶段 2：OpenAI Responses adapter

交付物：

- `/v1/responses` 请求构建；
- `response.output_text.delta`、completed、failed、incomplete、error 映射；
- response id、usage 与错误体解析；
- 默认 `store=false`，除非调用者明确选择并确认 provider 行为。

验收：与 OpenAI 官方 SDK相同输入时，文本拼接结果一致；401、429、5xx 与超时有稳定错误分类。

### 阶段 3：火山方舟 Responses adapter

交付物：

- `/api/v3/responses` 请求构建；
- 方舟官方流事件与错误对象映射；
- provider 特有参数留在适配层；
- 迁移期保留或删除旧 Chat adapter 的明确决定。

验收：分别与方舟官方 Ark SDK、OpenAI SDK兼容模式对照；中文、emoji、长文本和思考模型输出不丢字、不重复。

### 阶段 4：可靠性与资源验收

交付物：

- 解析器 fixture/Unity 测试；
- opt-in live integration test；
- 429/5xx 指数退避与抖动，仅在请求可安全重试时启用；
- 峰值 heap、最大连续内存、任务栈、首 token 延迟和总时延测量；
- 故障注入：断网、半包、服务端提前关闭、取消、OOM、无效 JSON。

验收：资源指标有基线和预算；网络失败不阻塞 LVGL；取消后资源全部释放；日志不含 Authorization 或完整响应敏感数据。

### 阶段 5：量产决策

在设备直连和网关之间做正式决策。若使用设备直连，需要书面接受 Key 可被提取的风险；若使用网关，则把 provider SDK、限流、审计与密钥轮换放入服务端设计。

## 8. 测试矩阵

| 类别 | 必测项 |
| --- | --- |
| SSE | CRLF/LF、任意分片、多行 data、空行派发、注释、超长事件、无终止事件 |
| JSON | 转义字符、中文、emoji/代理对、空 delta、未知字段、字段顺序变化、无效 JSON |
| OpenAI | created、text delta、completed、failed、incomplete、error、usage |
| 方舟 | 官方 Responses fixture、Chat 迁移 fixture、provider 错误体、兼容差异 |
| HTTP/TLS | 200、400、401、403、404、408、429、5xx、DNS、证书、read timeout |
| 生命周期 | 正常完成、用户取消、Wi-Fi 断开、重连、连续多请求、并发策略 |
| 资源 | TLS 握手峰值、SSE 峰值、cJSON 峰值、worker stack、泄漏检测 |
| 安全 | Key 不入库、不入日志、不进崩溃文本；endpoint 只允许 HTTPS |

## 9. 主要风险与取舍

| 风险 | 影响 | 缓解措施 |
| --- | --- | --- |
| 无官方 C++ SDK | 不能获得自动类型/升级能力 | provider adapter + 官方 SDK fixture 基准 |
| API 兼容不等于完全相同 | 共用解析器可能静默出错 | 两个 adapter，共享基础设施但不共享字段假设 |
| MCU 内存受限 | TLS + JSON + SSE 可能造成峰值/OOM | 流式解析、event 上限、测量后定预算、避免保存完整响应 |
| 设备保存长期 Key | 可被提取和滥用 | 量产使用网关；原型至少轮换、加密 NVS、日志脱敏 |
| 网络调用阻塞 UI | 动画卡顿、看门狗风险 | 独立 worker task + queue，不在回调操作 LVGL |
| provider 协议演进 | 固件解析失效 | 官方 SDK基准工具、fixture 回归、provider 能力版本化 |
| 自动重试导致重复副作用 | 工具调用或有状态请求重复 | MVP 只重试可证明安全的请求；记录 response id/状态 |

## 10. 本轮建议的最终决策

1. 保留 `otool_llm_sdk` 组件名和 C ABI，但把内部改成 provider adapter 架构。
2. 不引入任何非官方 C++ OpenAI 客户端。
3. OpenAI 与火山方舟的新接口都以 Responses API 为主，旧方舟 Chat Completions 只作迁移桥接。
4. 设备端使用 `esp_http_client + ESP certificate bundle + cJSON + SSE parser`。
5. 主机端使用 OpenAI 官方 SDK与 `volcengine-python-sdk[ark]` 生成一致性 fixture。
6. 在写双 provider 功能前，先完成 Key 轮换、联网和 TLS 校验。
7. 产品量产前优先选择官方 SDK网关，不在分发固件中放 provider 的长期 Key。

## 11. 官方资料

- [OpenAI SDKs and CLI](https://developers.openai.com/api/docs/libraries)：官方 SDK语言列表，也明确允许使用自选 HTTP client。
- [OpenAI：Migrate to the Responses API](https://developers.openai.com/api/docs/guides/migrate-to-responses)：新项目推荐 Responses API，并说明请求/响应与流式事件模型。
- [OpenAI Responses create reference](https://developers.openai.com/api/reference/cli/resources/responses/methods/create)：Responses 请求字段与 SSE 事件示例。
- [火山方舟：产品简介/快速开始](https://www.volcengine.com/docs/82379/1795150)：Responses API 的 Curl、Python、Go、Java 和 OpenAI SDK示例。
- [火山方舟：Responses 工具调用](https://www.volcengine.com/docs/82379/1958524?lang=zh)：官方 Responses 请求、响应与 SDK示例。
- [volcengine/volcengine-python-sdk](https://github.com/volcengine/volcengine-python-sdk)：官方 Python SDK，包含 `volcenginesdkarkruntime` 与 `[ark]` 安装入口。
- [Ark Runtime completion example](https://github.com/volcengine/volcengine-python-sdk/blob/master/volcenginesdkexamples/volcenginesdkarkruntime/completions.py)：官方方舟流式调用示例。
- [ESP-IDF ESP HTTP Client](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/protocols/esp_http_client.html)：HTTPS、certificate bundle、stream 与取消 API。
- [ESP-IDF 6.0 protocol migration](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/migration-guides/release-6.x/6.0/protocols.html)：内置 JSON 组件移除与 `espressif/cjson` 迁移方式。
