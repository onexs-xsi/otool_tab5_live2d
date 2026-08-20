# spec/format — 素材包 manifest（草案）

> 状态：draft。设备端 `load_package()` 在 S2 与 `tools/clip_packer` /
> `tools/asset_packer` 同步冻结；S0 仅做"文件存在且非空"占位校验。

## 1. 设计原则（可行性报告 §8.1 / §8.4 / §3.2）

- 只接受明确支持的 manifest major version；未知 major 直接拒绝。
- 越界 offset、尺寸不匹配、整数溢出、CRC 错误在**读取数据前**拒绝。
- CRC 只用于误码检测；生产包叠加 SHA-256 与签名/anti-rollback（§6.2）。
- 包声明的 moc 版本/feature 超出编译 profile → 任何大块分配前拒绝（§4.6）。

## 2. 草案字段（冻结时确定布局）

| 字段 | 语义 |
|---|---|
| magic / schema version | 包格式标识 |
| target | 分辨率档位（640×360 等）与像素格式 |
| 资源清单 | 每文件：偏移、长度、SHA-256/CRC、用途（clip/纹理/moc3/motion） |
| 片段表 | clip 播放器：clip id、帧偏移、PTS、循环点、可跳转点（§8.1） |
| 模型 profile | moc 版本字节、feature bitmap、hard limit 快照 |
| 签名 | 产品签名（S2 起） |

## 3. 解析器错误策略

- 未知 major / 越界 / 尺寸不匹配 / 溢出 / CRC 错 → 稳定错误码（error_codes.md），
  设备回退 Flash fallback（不白屏）。
