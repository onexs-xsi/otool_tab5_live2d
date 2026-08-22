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
- 豆包原生实时语音对话端点（openspeech 协议，与 OpenAI Realtime 不同）：
  `wss://openspeech.bytedance.com/api/v3/realtime/dialogue`，协议为
  `StartSession`/`dialog` 事件（非 session.update/response.create）；模型族
  常量如 `1.2.1.1`（O 2.0）、`2.2.0.0`（SC 2.0）。参考实现仅用于协议研读，
  不复制代码；
- 方舟 OpenAI 兼容 Realtime 端点（`ark.cn-beijing.volces.com/api/v3/realtime*`）
  实测 404，**确切路径待控制台/文档确认**（可能需在方舟控制台开通 Realtime
  服务并获取专用端点）；
- 探针脚本 `test_apps/ark_realtime_probe.py`（OpenAI 兼容协议框架）已提交，
  参数化 endpoint/model；**未通过前不写设备代码**；
- 待确认：Realtime 模型 ID、WSS 端点与鉴权（API Key 直连或临时 token）、
  音频格式（采样率/编码）、方舟兼容层的事件 schema。

## 验收

- [ ] 最小 WSS 探针（Python）：连接、鉴权、文本/事件往返、断线重连语义
- [ ] 半双工 MVP 设计（模块划分、buffer、VAD/打断、播放）提交到本 ADR
- [ ] 未通过前，`main/` 不出现语音相关代码，SDK 不出现 realtime 头文件
