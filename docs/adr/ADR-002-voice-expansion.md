# ADR-002：语音扩展采用半双工 ASR → Agent → TTS

状态：已接受，MVP 已实现（待火山账号与 Tab5 真机联调）

日期：2026-08-23

相关：`docs/volcengine_speech_integration.md`

## 背景

设备已经具备文本 Agent、工具调用、Tab5 四通道录音和双通道播放能力。当前目标是
接入火山引擎语音识别和语音播放，同时保持 ESP32-P4 上的内存上限、无密钥安全
启动，以及现有文本 Agent 的 provider/工具能力不变。

## 决策

采用可替换的半双工链路：

```text
ES7210 4ch PCM
  -> 选定单通道、16 kHz mono
  -> 火山大模型流式 ASR（WebSocket）
  -> agent_app_ask() / 现有文本 Agent 与工具闭环
  -> 火山单向流式 TTS（HTTP chunked JSON）
  -> Base64 PCM 解码、mono 转 stereo
  -> Tab5 I2S/Codec 播放
```

具体约束：

1. 新建独立 `components/otool_speech_sdk`，不把音频协议混入
   `otool_llm_sdk` 的文本 SSE/Agent Runtime。
2. ASR 使用火山优化版流式端点
   `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async`，输入固定为
   16 kHz、16-bit、signed little-endian、mono PCM，按 200 ms 聚包并 gzip。
3. Agent 回复使用单向流式 TTS HTTP
   `https://openspeech.bytedance.com/api/v3/tts/unidirectional`，请求 raw PCM，收到
   一个 JSON 音频块就解码并播放，不等待整段音频生成完毕。
4. 用户提供的“音频生成 HTTP”不是主对话 TTS 链路；保留为后续音效/定制音频
   能力候选，不在本次 MVP 中实现。
5. 录音和播放互斥，不做回声消除和全双工打断；这些能力必须在半双工真机稳定后
   再独立评估。
6. Speech API Key、ASR/TTS resource id 和 speaker id 由本地 `sdkconfig` 手工配置。
   空密钥禁止语音网络请求并禁用按钮，但不影响设备/UI 启动。

## 原因

- 复用已经具备工具调用能力的文本 Agent，不需要重新实现 Realtime 工具协议。
- 16 kHz 端到端不需要设备侧重采样；仅执行 4ch→mono 和 mono→stereo，CPU 与
  RAM 开销可控。
- ASR WebSocket 与 TTS HTTP 的生命周期互相独立，便于分别诊断鉴权、网络、
  识别、合成和 Codec 故障。
- TTS 使用 raw PCM 可直接交给现有 I2S 播放，无 WAV 头处理和整段缓存。

## 后果与限制

- 当前交互必须手动按键开始、再次按键停止；尚无 VAD、自动尾点或 barge-in。
- HTTP 回调当前对 I2S 写入施加背压；这是有界内存设计，但真机需要测量是否会
  触发网络超时。若发生，再在组件和驱动间增加有界 PCM 队列。
- `sdkconfig` 方式会把密钥编入固件，符合当前部署要求，但固件不是安全密钥仓库；
  `sdkconfig` 和生成固件不得公开发布。
- ASR 成功而文本 Agent 未配置时，只展示识别文本和安全错误，不调用 TTS。
- 未配置 TTS speaker 时，ASR 和文本 Agent 仍可运行，仅跳过语音播放。

## 后续决策门

只有以下项目在真机通过后，才评估全双工 Realtime：

- 连续 20 轮 ASR → Agent → TTS 无崩溃、无任务/连接泄漏；
- 测得 ASR 最终结果、Agent 首 token、TTS 首音频三段延迟；
- 确认最合适的 ES7210 麦克风通道和增益；
- 断网、无权限、配额耗尽、空识别、TTS 中断均可恢复到 READY；
- 峰值 internal heap、PSRAM 和任务栈余量达到发布门槛。
