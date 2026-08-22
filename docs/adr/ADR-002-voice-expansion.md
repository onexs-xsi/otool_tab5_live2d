# ADR-002：语音扩展方案（WP10）

状态：草稿（规划；文本 Agent MVP 已稳定后评估）
日期：2026-08-22
相关：docs/otool_llm_sdk_agent_implementation_plan.md §WP10

## 背景

文本 Agent（Responses/Chat 双协议 + 工具闭环）已在真机稳定（WP6/WP8/WP9）。
下一步语音扩展存在两条路线，本文记录决策框架与约束；**在 ADR 定稿和最小
探针通过前，不把任何语音代码混入 Agent MVP**（§WP10 Gate）。

## 候选方案

### A. 半双工链路（推荐先做 MVP）

ASR → 文本 Agent（现有 `otool_llm_text.h` + agent runtime，零改动）→ TTS

- 优点：完全复用现有 SDK 与真机验证过的 Agent 状态机/工具/取消；增量小；
  故障域清晰（三段独立可替换）；调试与现有 console 工作流一致。
- 缺点：端到端延迟 = ASR 尾点 + 模型首 token + TTS 首帧，存在"打断"窗口；
  需要 VAD/尾点检测与播放打断逻辑。
- 组件：麦克风采集（Tab5 自带麦克风/PDM）→ ASR（本地或云端 HTTP）→
  文本注入 `llm_app_ask_text()` 同构入口（agent 已有 `agent_app_ask`）→
  回复文本 → TTS（云端 HTTP → I2S/Codec 播放）。
- 新增 API 形态建议：`otool_llm_voice.h`（可选）只做胶水，不碰 text 协议。

### B. 全双工 Realtime WebSocket

- 优点：单连接、低首帧延迟、支持打断（barge-in）。
- 缺点：新传输（WSS）、新协议（OpenAI Realtime 事件；Ark 需确认兼容模型与
  鉴权）、工具调用映射要重新验证（Realtime 的 function_call 事件）、
  VAD/回声消除/半双工音频管理复杂、真机调试成本高。
- 约束：若实施，新增 `otool_llm_realtime.h`，**禁止污染 `otool_llm_text.h`**；
  tool registry/schema 校验/取消语义必须与文本 Agent 对齐（同一
  `otool_llm_tool_definition_t` 与 `otool_llm_tool_registry_t`）。

## 决策

1. 先做方案 A（半双工）MVP：复用现有 Agent，先解决"采集→ASR→文本→TTS→
   播放"最小闭环与中断语义，**不引入新协议面**。
2. 方案 B（Realtime）仅作为后续研究项；**Gate 前先提交最小 WSS 探针**
   （本机 Python 脚本，验证鉴权、事件往返、音频帧格式），不写设备代码。
3. 语音 API 凭证与文本 API 分开管理（NVS `otool_cred` 增加
   `asr_key`/`tts_key` 或复用 `llm_key` 的评估结论待探针确定）。

## 事实核查（2026-08-22）

- 火山方舟已提供 **Realtime API 调用 Doubao**（官方文档：
  [使用 Realtime API 调用 Doubao](https://www.volcengine.com/docs/6893/1527770?lang=zh)、
  [建连参数](https://docs.volcengine.com/docs/6893/1527759?lang=zh)），
  全双工 WSS 方案在方舟上可行；
- 社区有豆包 s2s（speech-to-speech）示例仓库（如
  [doubao-s2s-example](https://github.com/openqht/doubao-s2s-example)）可参考
  协议细节，但本项目不复制其代码；
- 探针需确认：Realtime 模型 ID（预置模型列表）、WSS 端点与鉴权方式
  （API Key 直连或临时 token）、音频格式（采样率/编码）、事件 schema
  （session.update / conversation.item / response.create 等 OpenAI 兼容性）。

## 验收

- [ ] 最小 WSS 探针（Python）：连接、鉴权、文本/事件往返、断线重连语义
- [ ] 半双工 MVP 设计（模块划分、buffer、VAD/打断、播放）提交到本 ADR
- [ ] 未通过前，`main/` 不出现语音相关代码，SDK 不出现 realtime 头文件
