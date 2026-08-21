# ESP-Claw 直接组件化集成研究与实施计划

> 状态：可行性已通过构建验证，待产品能力取舍确认后实施  
> 调研日期：2026-08-21  
> 目标平台：ESP32-P4 / ESP-IDF 6.1-beta1  
> 上游快照：`espressif/esp-claw@fb7b248114bb1b12ba0fe8e03d4b59bdbec292c1`  
> 范围：研究、最小构建验证与集成计划；本轮不改业务代码

> **决策已更新（2026-08-22）：** 由于逐增量流式输出是硬需求，且后续可能接入豆包语音，本项目不执行本文的直接替换路线。ESP-Claw 仅作为 provider、TLS、错误和取消设计参考。权威实施计划见 [`otool_llm_sdk_streaming_voice_implementation_plan.md`](./otool_llm_sdk_streaming_voice_implementation_plan.md)。

## 1. 结论

**可以直接复用 ESP-Claw，并可在接受其当前能力边界时替换 `components/otool_llm_sdk`。**

推荐方式不是把整个 ESP-Claw 应用移植进来，也不是等待一个不存在的组件注册表包，而是：

1. 将官方 `espressif/esp-claw` 仓库作为固定提交的 Git submodule 放在 `third_party/esp-claw`；
2. 只把 `claw_core` 及其最小依赖闭包加入 `EXTRA_COMPONENT_DIRS`；
3. 业务代码直接调用公开的 `claw_core.h` C API；项目是 C++，可通过该头文件的 `extern "C"` 直接使用；
4. OpenAI 与火山方舟均先走 ESP-Claw 的 `openai_compatible` Chat Completions 后端；
5. 在双提供商真机验收通过后，再移除 `components/otool_llm_sdk`。

这个方案已经通过本机最小构建探针，而不只是源码推断：

- ESP-IDF：`v6.1-beta1`；
- Target：`esp32p4`；
- 实际引用：`claw_core_create/start/submit/receive/stop/destroy`；
- 结果：编译、最终链接和固件生成成功；
- 探针固件：`0xa99a0` 字节，1 MiB app 分区仍余约 34%。

但是，当前 ESP-Claw **不能等价替代原计划中的流式 Responses 客户端**：

- 当前 OpenAI-compatible 后端固定调用 `/chat/completions`；
- HTTP 层先把完整响应收进内存，再解析 `choices[0].message`；
- `claw_core_receive*()` 只返回最终文本，没有 token/delta 回调；
- 不支持 OpenAI `/responses` 事件模型；
- 暂未暴露 `temperature` 等每请求采样参数；
- 响应缓冲从 4096 字节起倍增，目前没有最大响应上限。

因此本报告给出的最终判断是：

- **若当前目标是尽快获得 OpenAI/方舟兼容调用、异步 Agent worker、取消、工具调用扩展能力，可以直接采用 ESP-Claw，并删除自研 SDK。**
- **若 Live2D 产品必须逐 token 展示、必须使用 Responses API，当前上游不能直接满足，暂时不能删除流式实现。**

当前 `main` 尚未真正调用 `otool_llm_sdk`，只在 CMake 中声明依赖，所以现在切换成本最低。

## 2. 能力匹配表

| 需求 | ESP-Claw 当前能力 | 判断 |
| --- | --- | --- |
| ESP32-P4 | 上游支持 P4 板卡；本项目环境已实编通过 | 满足 |
| ESP-IDF 6.1 | 上游 release 声明兼容 IDF v6；本机 `6.1-beta1` 已实编通过 | 满足 |
| C++ 工程调用 | `claw_core.h` 为 C ABI，并提供 `extern "C"` | 满足 |
| OpenAI 官方服务 | `openai_compatible` + `/chat/completions` | 满足 Chat Completions |
| 火山方舟 | 自定义 base URL + Bearer + OpenAI-compatible JSON | 高概率满足，仍需真机 smoke test |
| 自定义 OpenAI-compatible endpoint | base URL、认证类型、模型、max-token 字段可配置 | 满足 |
| 独立 worker task | `claw_core_start()` 创建 FreeRTOS Agent task | 满足 |
| 请求队列与结果队列 | `submit` + `receive/receive_for` | 满足 |
| 取消 | `claw_core_cancel_request()` + HTTP abort flag | 满足 |
| HTTPS 服务端校验 | HTTP transport 已使用 `esp_crt_bundle_attach` | 满足 |
| 工具调用 | Core 有 tool-call loop 和 `call_cap` 回调 | 可选满足 |
| 图片输入 | 后端与 media pipeline 有 vision 支持 | 可选满足，需资源测试 |
| 多轮会话记忆 | Core 支持 context provider/persist callback；最小集成本身不带 `claw_memory` | 需要应用补接 |
| 文本 delta/SSE | 完整响应缓冲后返回最终文本 | 不满足 |
| OpenAI Responses API | 当前没有 `/responses` adapter | 不满足 |
| 严格内存上限 | 完整响应缓冲无 cap | 不满足，必须压测或上游修复 |
| 单个注册表组件安装 | `claw_core` 未作为独立版本化包发布 | 不满足，采用 Git submodule |

## 3. 最小上游组件闭包

不能只复制 `components/claw_modules/claw_core`。它的 CMake/manifest 形成如下依赖闭包：

```text
claw_core
├── claw_utils
├── http_reuse
├── espressif/cjson（组件管理器下载）
└── claw_event_router
    ├── claw_cap
    │   └── claw_skill
    └── claw_manager
```

需要加入构建搜索路径的 7 个上游目录：

```text
components/claw_modules/claw_core
components/claw_modules/claw_utils
components/claw_modules/claw_event_router
components/claw_modules/claw_cap
components/claw_modules/claw_manager
components/claw_modules/claw_skill
components/common/http_reuse
```

不要把 ESP-Claw 的 `components/common`、`components/claw_modules` 或整个 `components` 目录笼统加入搜索路径。这样会让 ESP-IDF 组件管理器扫描 `app_claw`、UI、Lua builder、板级和其他无关 manifest，扩大依赖解析范围。

也不需要引入：

- `application/edge_agent`；
- `app_claw`；
- Lua runtime 与 `lua_module_*`；
- 音频、相机、IM 平台；
- 具体 `cap_*` 能力组件；
- ESP-Claw 的 FATFS 镜像和 Web 配置页面。

上游采用 Apache-2.0，直接复用时需要保留其 LICENSE、源码 SPDX 头和第三方声明。

## 4. 源码管理与 CMake 方案

### 4.1 固定上游版本

建议目录：

```text
third_party/
└── esp-claw/   # Git submodule，固定到评审通过的 commit/tag
```

首个集成基线固定为本次验证的 commit：

```text
fb7b248114bb1b12ba0fe8e03d4b59bdbec292c1
```

不要在构建时自动跟随 `master`。ESP-Claw 当前版本仍为早期 `v0.1.0`，近期 changelog 中存在 breaking change；升级必须通过单独 PR、双提供商协议测试和固件资源回归。

### 4.2 根 CMake

在 `include($ENV{IDF_PATH}/tools/cmake/project.cmake)` 之前加入精确目录：

```cmake
set(ESP_CLAW_ROOT "${CMAKE_SOURCE_DIR}/third_party/esp-claw")

list(APPEND EXTRA_COMPONENT_DIRS
    "${ESP_CLAW_ROOT}/components/claw_modules/claw_core"
    "${ESP_CLAW_ROOT}/components/claw_modules/claw_utils"
    "${ESP_CLAW_ROOT}/components/claw_modules/claw_event_router"
    "${ESP_CLAW_ROOT}/components/claw_modules/claw_cap"
    "${ESP_CLAW_ROOT}/components/claw_modules/claw_manager"
    "${ESP_CLAW_ROOT}/components/claw_modules/claw_skill"
    "${ESP_CLAW_ROOT}/components/common/http_reuse"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(otool_tab5_live2d)
```

所有本地 path 依赖应与工程保持在同一盘符。本次探针确认，工程在 D: 而上游临时 clone 在 C: 时，IDF 6.1 组件管理器会在 lockfile 序列化阶段产生跨盘 path 错误；放入工程内的 `third_party` 可规避此问题。

### 4.3 `main/CMakeLists.txt`

把：

```cmake
otool_llm_sdk
```

替换为：

```cmake
claw_core
```

`espressif/cjson` 会由 `claw_core/idf_component.yml` 解析。提交时一并审查 `dependencies.lock` 变化。

### 4.4 HTTP 连接复用

第一阶段保持 `CONFIG_HTTP_REUSE_ENABLE=n`：

- 开启后，`http_reuse` 通过 linker `--wrap` 全局接管 `esp_http_client_init/cleanup/perform`；
- 它会影响工程内所有 `esp_http_client` 使用者，而不仅是 LLM；
- 对复用连接失败的一次自动重试可能带来重复计费或重复工具调用风险。

完成多模块回归后，再决定是否开启连接池。

## 5. 直接调用方式

### 5.1 生命周期

```text
网络就绪
  -> claw_core_create(config)
  -> claw_core_start()
  -> claw_core_submit(request)
  -> claw_core_receive_for(request_id)
  -> UI queue 接收最终文本

关机/切换服务
  -> claw_core_cancel_request()
  -> claw_core_stop()
  -> claw_core_destroy()
```

`claw_core` 自带请求队列和 worker task，因此不需要再为 HTTP 调用创建一套 `otool_llm_sdk` worker。网络初始化、凭证读取、UI 状态与结果显示仍属于本应用。

### 5.2 Provider 配置

OpenAI：

```c
.backend_type = "openai_compatible",
.base_url = "https://api.openai.com/v1",
.auth_type = "bearer",
.max_tokens_field = "max_completion_tokens",
.model = "<OpenAI model>",
```

火山方舟：

```c
.backend_type = "openai_compatible",
.base_url = "https://ark.cn-beijing.volces.com/api/v3",
.auth_type = "bearer",
.max_tokens_field = "max_tokens",
.model = "<Ark endpoint/model ID>",
```

ESP-Claw 会在 base URL 后追加 `/chat/completions`。火山配置需要用轮换后的 Key 做一次真实请求验证，重点确认：

- 模型/endpoint ID 的填写方式；
- `max_tokens` 字段是否被当前模型接受；
- `reasoning_content`、工具调用和错误体形状；
- HTTP 429/5xx 行为；
- 中文、emoji 和长输出。

### 5.3 简单聊天模式

第一阶段建议：

- `.supports_tools = false`；
- `.supports_vision = false`；
- `.call_cap = NULL`；
- `.persist_context = NULL`；
- `.max_context_providers = 0`；
- 请求/响应队列长度从 1 开始；
- task stack 放 PSRAM，实测后确定，不直接照抄上游完整 Agent 的大栈配置。

这样把 ESP-Claw 先作为异步 OpenAI-compatible Chat 客户端使用，避免一次带入工具、记忆和媒体复杂度。

多轮历史不是最小模式自动拥有的能力。若后续需要，可按优先级选择：

1. 应用实现轻量 context provider；
2. 再评估引入 `claw_memory`；
3. 不建议在 UI 层自行拼接无限增长的完整历史。

## 6. 资源结果与风险

### 6.1 构建探针结果

| 项目 | 结果 |
| --- | --- |
| 类型引用、未启动 Core 的探针固件 | `0x2e720` bytes |
| 实际 start/submit/receive 路径探针固件 | `0xa99a0` bytes |
| 差值 | `0x7b280` bytes，约 493 KiB |
| app 分区 | 1 MiB，实际调用探针剩余约 34% |

约 493 KiB 的差值不是纯 ESP-Claw 框架开销，它还包含首次真正拉入的 HTTPS/TLS、HTTP client、JSON 和网络代码。任何设备直连 LLM 的实现都会承担其中大部分成本。

实际被链接的主要 ESP-Claw/相关 archive 贡献为：

| Archive | ELF contribution |
| --- | ---: |
| `libclaw_core.a` | 28,840 bytes |
| `libespressif__cjson.a` | 6,820 bytes |
| `libhttp_reuse.a` | 3,458 bytes |
| `libclaw_event_router.a` | 1,142 bytes |
| `libclaw_utils.a` | 350 bytes |

真实项目已有 LVGL、BSP 和部分网络组件，最终增量必须在本项目完成集成后重新测量，不能直接套用空工程差值。

### 6.2 运行时内存

当前 HTTP transport 的主要特征：

- RX/TX buffer 各配置 4096 字节；
- 响应缓冲初始 4096 字节，空间不足时倍增；
- 请求 JSON 会额外复制一份用于 UTF-8 清洗；
- 完整响应再进入 cJSON DOM 解析；
- Core 还会创建 worker task、两个 queue 和多个 mutex。

因此长输出峰值可能同时存在“响应原文 + 扩容余量 + cJSON DOM + 最终文本”。第一阶段必须限制 `max_tokens`，并记录请求前、TLS 后、收到响应后和解析后的最小 free heap/最大连续块。

上游若不增加响应字节上限，不能直接宣称满足量产内存安全要求。

### 6.3 API 与维护风险

- 上游 `v0.1.0` 尚早，公开结构体未来仍可能变化；
- LLM runtime 的更底层头文件位于 `src/llm`，不是稳定公共 API，业务只应包含 `claw_core.h`；
- 不复制/调用内部 `claw_llm_runtime_*`，否则实际上形成私有 fork；
- 更新上游时审查 `claw_core.h`、OpenAI-compatible backend、HTTP transport、license 和依赖 lock；
- 如果必须修改上游，优先提交 upstream PR；本地 patch 应集中、可重放，并记录原因。

## 7. 与 `otool_llm_sdk` 的替换边界

ESP-Claw 可接管：

- OpenAI-compatible 请求 JSON；
- Bearer/API-Key 认证；
- HTTPS certificate bundle；
- HTTP 错误解析；
- worker task；
- 请求/响应队列；
- 取消；
- 最终文本和工具调用解析。

应用仍需负责：

- ESP32-P4 的 Wi-Fi/网络协处理器初始化；
- IP、DNS、SNTP/时间就绪门禁；
- API Key 的运行时配置和安全存储；
- provider preset 与模型选择；
- `claw_core_response_t` 到 Live2D/LVGL 状态机的 queue 映射；
- 多轮上下文策略；
- 429/5xx 产品级退避；
- 指标、日志脱敏和资源监控。

删除 `components/otool_llm_sdk` 的验收门槛：

1. OpenAI 与方舟至少各完成一次真实 HTTPS 请求；
2. UI 不被网络阻塞；
3. 取消、断网、401、429、5xx 和超时能恢复；
4. 连续 20 次短请求无 heap 持续下降；
5. 最长允许输出不 OOM；
6. 产品明确接受“最终结果一次性返回”，或上游已经补齐流式事件；
7. 仓库、固件日志和 crash 文本中没有真实 Key。

验收前只从 `main/CMakeLists.txt` 停用旧依赖，不立即物理删除目录，便于 A/B 对照；验收后再删除。

## 8. 分阶段实施计划

### 阶段 0：安全与基线

- 立即撤销/轮换当前写在 `main/CMakeLists.txt` 中的方舟 Key；
- 从 CMake 编译宏移除长期 Key；
- 记录切换前固件大小、free heap、最大连续块和任务栈；
- 明确第一阶段是否接受非流式 UI。

### 阶段 1：上游固定与构建接入

- 添加 `third_party/esp-claw` submodule；
- 固定到本报告验证 commit；
- 根 CMake 只注册 7 个最小组件目录；
- `main` 依赖切为 `claw_core`；
- 保持 `HTTP_REUSE_ENABLE=n`；
- 在现有 ESP32-P4 工程完成 clean build 和 size report。

### 阶段 2：最小 Chat 调用

- 通过运行时配置创建 `claw_core`；
- 工具、vision、memory 全部关闭；
- 为 OpenAI 与方舟建立 provider preset；
- `submit/receive_for` 放入应用服务层；
- 结果通过 FreeRTOS queue 交给 UI，禁止 Core task 直接操作 LVGL。

### 阶段 3：双 Provider 与故障验收

- OpenAI/方舟真实 smoke test；
- 401、429、5xx、DNS、TLS、timeout、cancel；
- 中文/emoji/长输出；
- 连续请求和 provider 热切换；
- 收集 request id 时必须脱敏，不记录 Authorization。

### 阶段 4：资源与体验决策

- 测量 task stack high-water mark、峰值 heap、最大连续块；
- 根据测试设置 max tokens 和上下文上限；
- 评审“一次性最终回复”是否满足 Live2D 体验；
- 若满足，正式删除 `otool_llm_sdk`；
- 若不满足，向 ESP-Claw 增加公开流式事件 API并争取 upstream，或保留专用流式组件。

### 阶段 5：可选 Agent 能力

只有产品确实需要时，再按顺序加入：

1. context provider；
2. 持久会话；
3. 工具调用与 capability bridge；
4. vision；
5. HTTP 连接复用。

每加入一项都重新做内存、取消和故障回归。

## 9. 最终建议

本项目当前建议采用 **ESP-Claw Core 直接复用路线**，理由是：

- 上游由 Espressif 维护、Apache-2.0；
- P4/IDF 6.1 已用实际构建验证；
- 当前业务还没有绑定旧 SDK API，切换时机好；
- Core 已提供 worker、队列、取消、provider 配置、TLS 与未来工具扩展；
- 可以避免继续维护一套重复的 Chat Completions HTTP 客户端。

此决定附带一个明确的产品条件：**第一阶段接受非流式 Chat Completions**。如果后续把逐 token Live2D 表现或 OpenAI Responses 定为硬要求，需要推动 ESP-Claw 上游增加能力，而不是假定它现在已经支持。

## 10. 参考资料

- [espressif/esp-claw](https://github.com/espressif/esp-claw)：上游仓库、许可证和总体架构。
- [ESP-Claw v0.1.0 release](https://github.com/espressif/esp-claw/releases/tag/v0.1.0)：P4 与 ESP-IDF v5.5.4/v6 支持说明。
- [claw_core public API](https://github.com/espressif/esp-claw/blob/master/components/claw_modules/claw_core/include/claw_core.h)：Core 生命周期、队列、取消与配置结构。
- [claw_core CMake](https://github.com/espressif/esp-claw/blob/master/components/claw_modules/claw_core/CMakeLists.txt)：源文件与直接组件依赖。
- [OpenAI-compatible backend](https://github.com/espressif/esp-claw/blob/master/components/claw_modules/claw_core/src/llm/backends/claw_llm_backend_openai_compatible.c)：Chat Completions 请求与最终 JSON 解析。
- [LLM HTTP transport](https://github.com/espressif/esp-claw/blob/master/components/claw_modules/claw_core/src/llm/claw_llm_http_transport.c)：证书包、完整响应缓冲、取消和错误处理。
- [OpenAI Responses migration guide](https://developers.openai.com/api/docs/guides/migrate-to-responses)：用于说明 Responses 与 Chat Completions 是不同协议层，不应混为同一兼容实现。
- [火山方舟官方文档](https://www.volcengine.com/docs/82379/1795150)：方舟 API 与 OpenAI SDK 兼容调用基准。
