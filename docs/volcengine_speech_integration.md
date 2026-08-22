# 火山引擎语音 2.0 接入与真机验证报告

日期：2026-08-23

设备：M5Stack Tab5（ESP32-P4，32 MB PSRAM），COM3

范围：流式语音识别 2.0、双向流式语音合成 2.0、Agent 语音数据链路；不包含麦克风录音质量和扬声器听感验收

## 1. 结论

当前 Agent 主链路已经统一使用火山引擎语音平台，不再依赖 Ark 大模型推理平台的音频接口：

```text
ES7210 4 通道 PCM
  -> 选择一路 16 kHz/16-bit/mono PCM
  -> 火山流式语音识别 2.0（WebSocket）
  -> 增量/最终文本
  -> 现有 Agent + 工具调用
  -> 火山语音合成 2.0（双向 WebSocket）
  -> 流式 PCM
  -> Tab5 播放
```

接口、鉴权、协议帧和 PCM 数据均已在主机侧及 COM3 真机验证。真机三项探针全部通过：

- ASR 发送 1 秒静音：`ESP_OK`；
- TTS 返回约 3.4 秒、109112 字节 16 kHz/16-bit/mono PCM；
- TTS -> ASR 内存回环：10 次增量文本更新，最终中文与输入逐字一致，`exact_match=1`。

此前 ASR 在 WebSocket 握手后返回 `ESP_ERR_NO_MEM` 的问题已经解决。根因是 miniz 的高级压缩函数会在普通堆中临时创建约百 KB 级压缩状态，而此时 TLS/WebSocket 已占用大量内部 RAM。现在压缩状态及大块顺序缓冲均优先放入 PSRAM，普通 8-bit heap 只作为无 PSRAM 设备的回退。

## 2. 官方接口基线

以最新官方文档为准：

- [流式语音识别 2.0](https://docs.volcengine.com/docs/6561/2630027?lang=zh)
- [语音合成 2.0 双向流式接口](https://docs.volcengine.com/docs/6561/2532486?lang=zh)

### 2.1 ASR

| 项目 | 当前值 |
|---|---|
| Endpoint | `wss://openspeech.bytedance.com/api/v3/sauc/bigmodel_async` |
| 按时长 Resource ID | `volc.seedasr.sauc.duration` |
| 并发 Resource ID | `volc.seedasr.sauc.concurrent`（仅并发套餐使用） |
| 鉴权头 | `X-Api-Key`、`X-Api-Resource-Id`、`X-Api-Request-Id`、`X-Api-Sequence: -1` |
| 输入 | PCM，16 kHz，16-bit，mono，小端有符号整数 |
| 分包 | 200 ms（组件可配置为 100–200 ms） |
| 响应 | partial/final 文本回调 |

服务端实测要求初始请求使用 `audio.format="pcm"`、`audio.codec="raw"`。`format="raw"` 会被当前服务以“不支持的音频格式”拒绝，因此代码保留已通过真实接口验证的 `pcm` 值。

### 2.2 TTS

| 项目 | 当前值 |
|---|---|
| Endpoint | `wss://openspeech.bytedance.com/api/v3/tts/bidirection` |
| Resource ID | `seed-tts-2.0` |
| 鉴权头 | `X-Api-Key`、`X-Api-Resource-Id`、`X-Api-Connect-Id` |
| 输出 | PCM，默认 16 kHz，16-bit，mono |
| 会话 | StartConnection -> StartSession -> TaskRequest -> FinishSession -> FinishConnection |

实现按当前线上接口实际返回的 full-server `0x9`、audio-server `0xB` 消息类型解析，并处理 Connection/Session/Audio/Failure 事件。旧示例仓库中的历史数字不能直接照搬。

## 3. 组件实现

| 文件 | 职责 |
|---|---|
| `components/otool_speech_sdk/include/otool_speech_sdk.h` | ASR/TTS 公共 C API、默认 Endpoint、PCM 工具 |
| `components/otool_speech_sdk/src/volcengine_asr.c` | ASR WebSocket 会话、gzip 协议帧、PCM 分包、partial/final 解析 |
| `components/otool_speech_sdk/src/volcengine_tts.c` | TTS 双向 WebSocket 状态机、事件帧、流式 PCM 回调 |
| `components/otool_speech_sdk/src/gzip_codec.c` | 无隐式堆分配的低层 miniz 压缩/解压封装 |
| `components/otool_speech_sdk/src/speech_memory.c` | PSRAM 优先、普通 8-bit heap 回退的大块分配器 |
| `main/ui_app.cpp` | 录音、识别、Agent、播放状态机 |
| `main/console_cmds.cpp` | ASR、TTS、TTS->ASR 无物理音频探针 |

`volcengine_asr_file.c` 仍作为旧录音文件 URL 接口的兼容实现保留，但不参与当前 UI/Agent 主链路，也不是本阶段的验收对象。

### 3.1 PSRAM 策略

以下对象优先使用 `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`：

- ASR 200 ms PCM 聚合缓冲；
- ASR gzip 发送缓冲；
- ASR/TTS WebSocket 完整消息缓冲；
- ASR/TTS gzip 解压输出缓冲；
- miniz `tdefl_compressor` 和 `tinfl_decompressor` 状态；
- TTS JSON 大文本输出和二进制事件帧；
- 控制台回环 PCM 与 ASR 静音测试数据。

以下对象仍放内部 RAM：

- WebSocket/TLS 库自身要求的控制状态；
- FreeRTOS 信号量、锁和小型 session；
- API Key 请求头（体积小，释放前清零）；
- 最终文本小缓冲及高频小状态。

所有 PSRAM 分配失败时都会回退普通 8-bit heap，因此组件仍可在无 PSRAM 的 ESP-IDF 目标上工作；若两者都失败则返回 `ESP_ERR_NO_MEM`，不会崩溃。

## 4. sdkconfig

通过 `menuconfig` 配置真实值：

```text
CONFIG_OTOOL_SPEECH_API_KEY="..."
CONFIG_OTOOL_SPEECH_ASR_RESOURCE_ID="volc.seedasr.sauc.duration"
CONFIG_OTOOL_SPEECH_TTS_RESOURCE_ID="seed-tts-2.0"
CONFIG_OTOOL_SPEECH_TTS_SPEAKER="..."
```

Speech API Key 与 Ark LLM Key 是不同凭证。真实凭证只进入本地、Git 忽略的 `sdkconfig`，不得写入 `sdkconfig.defaults`、源码、文档或日志。缺少 Key/Resource/Speaker 时，语音能力返回可处理的配置错误并保持系统运行，不触发 abort。

关键容量配置：

```text
CONFIG_OTOOL_SPEECH_ASR_PACKET_MS=200
CONFIG_OTOOL_SPEECH_ASR_MAX_RESPONSE_BYTES=16384
CONFIG_OTOOL_SPEECH_TTS_STREAM_BUFFER_BYTES=32768
CONFIG_OTOOL_SPEECH_WS_TASK_STACK_SIZE=8192
CONFIG_WS_BUFFER_SIZE>=4096
```

## 5. 编译、烧录与验证

环境：ESP-IDF `v6.1-beta1`，目标 `esp32p4`。Windows 上使用 `ninja -j2` 避免高并发工具链进程启动失败。

构建结果：

```text
otool_tab5_live2d.bin size: 0x8ca4e0
smallest app partition free: 0x635b20 (41%)
```

固件已写入 COM3，写入数据 hash 校验通过并硬复位。设备检测到 32 MB PSRAM，Wi-Fi、显示和音频编解码器初始化正常。

### 5.1 ASR 静音探针

命令：

```text
speech-asr-probe
```

结果：

```text
ASR open=ESP_OK
ASR silence result=ESP_OK updates=0 text_bytes=0
PASS mode=asr-silence total_ms=1513
```

静音无文本是正确结果；重点是握手、初始 gzip 请求、PCM 分包、final 包和最终响应均完成。

### 5.2 TTS 探针

命令：

```text
speech-tts-probe
```

结果：

```text
TTS result=ESP_OK chunks=10 pcm_bytes=109112 duration_ms=3409
peak=14101 mean_abs=1639
PASS total_ms=1471
```

PCM 字节数为偶数、峰值非零，证明收到有效 16-bit PCM 数据，而不是空包或仅有控制事件。

### 5.3 TTS -> ASR 回环

命令：

```text
speech-loopback
```

该探针只在 PSRAM 中保存 TTS PCM，再把它馈入 ASR，不播放扬声器，也不读取麦克风。

```text
TTS result=ESP_OK chunks=9 pcm_bytes=108480 duration_ms=3390
ASR open=ESP_OK
ASR partial ...
ASR final text=你好，我正在进行语音接口回环测试。
ASR result=ESP_OK updates=10 text_bytes=51 exact_match=1
PASS total_ms=5839
```

这项测试同时覆盖 TTS 控制状态机、PCM 数据、ASR 分包、增量结果和最终结果，是本阶段“不做实际物理音频测试”条件下的端到端验收。

## 6. 尚未验收

- ES7210 四通道中最佳麦克风通道、增益、噪声和回声；
- 真实说话时的断句、录音时序和 UI 动画体验；
- 扬声器响度、爆音、欠载和听感；
- 录音与播放并发时的回声消除；
- 长文本连续 TTS 与长时间 ASR 的压力、断网重连和配额耗尽行为。

下一阶段可直接从 UI 实际录音与播放验收开始，不再需要更换语音平台或协议。
