# otool_speech_sdk

ESP-IDF speech transport for the OTool Tab5 voice Agent.

Current capabilities:

- Volcengine recording-file ASR 2.0 over HTTP submit/query polling;
- public audio URL validation, provider status parsing, bounded JSON responses,
  and explicit `ESP_ERR_NOT_ALLOWED` mapping for resource authorization errors;
- Volcengine BigModel streaming ASR over binary WebSocket;
- 16 kHz, signed 16-bit, mono PCM input with bounded 100–200 ms packets;
- gzip request/response payloads and fragmented WebSocket response assembly;
- Volcengine unidirectional HTTP streaming TTS with incremental JSON parsing;
- Base64 raw PCM callbacks;
- interleaved 4-channel capture to mono and mono to stereo helpers;
- safe `ESP_ERR_INVALID_STATE` result when credentials are missing.

The recording-file API accepts only an HTTP(S) URL which Volcengine can fetch.
It does not accept PCM or Base64 in the request body. An embedded application
must therefore publish the completed WAV/MP3 through object storage or its own
backend before calling `otool_speech_asr_file_recognize_url()`.

The Volcengine WebSocket upgrade response can exceed ESP-IDF's default 1 KiB
transport handshake buffer. Applications must configure
`CONFIG_WS_BUFFER_SIZE` to at least 4096 bytes; this project uses 8192 bytes and
enables `CONFIG_WS_DYNAMIC_BUFFER` so the allocation is released after connect.

See `docs/volcengine_speech_integration.md` for architecture, local sdkconfig
settings and the hardware test checklist.
