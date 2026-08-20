# otool_cubism_tool

M5Stack Tab5（ESP32-P4）上的 Live2D/Cubism 领域组件 —— 唯一承载 Cubism 能力的
ESP-IDF 组件。`main` 只负责创建板级对象、注入端口并选择运行模式。

> **主路线（2026-08-21 调整）**：组件内**自研 moc3/Core 兼容运行时**
> （`SELF_CORE`，可行性报告路线 E）+ CPU 软光栅。官方 Core 与第三方
> 重实现仅作为有来源记录的研究/oracle 角色。
> 实现状态：S0 骨架 + 研发治理骨架（`research/`、`spec/`、`test/vectors/`）。

## 运行模式

| 模式 | 对应路线 | 状态 |
|---|---|---|
| `CLIP_PLAYER` | D 预渲染片段播放 | Kconfig 默认开；播放器实现 S2 |
| `STREAM_CLIENT` | C 主机串流 | 默认关；实现 S2b |
| `REALTIME` | E 板端实时（self Core） | 默认关；**Gate 通过前开启会构建失败** |

## Kconfig

| 配置 | 默认 | 说明 |
|---|---|---|
| `CONFIG_OTOOL_CUBISM_ENABLE_CLIP_PLAYER` | y | 预渲染播放模式（安全网/fallback） |
| `CONFIG_OTOOL_CUBISM_ENABLE_STREAM_CLIENT` | n | 主机串流模式 |
| `CONFIG_OTOOL_CUBISM_ENABLE_REALTIME` | n | 板端实时；开启即 FATAL（未实现） |
| `CONFIG_OTOOL_CUBISM_CORE_BACKEND_*` | NONE | NONE / **SELF**；REALTIME 必须 SELF |
| `CONFIG_OTOOL_CUBISM_MOC_PROFILE_V5` | y | 首版唯一可发布 moc3 profile（版本字节 5） |
| `CONFIG_OTOOL_CUBISM_ANIMATION_BACKEND_SELF` | y | 动画后端：组件自研（默认） |
| `CONFIG_OTOOL_CUBISM_ANIMATION_BACKEND_APPROVED_FRAMEWORK` | n | 获准 Framework 复用（需审批 + csm shim） |
| `CONFIG_OTOOL_CUBISM_ENABLE_CSM_COMPAT` | n | 私有 `csm*` shim（仅 Framework adapter 用） |
| `CONFIG_OTOOL_CUBISM_ENABLE_V6_OFFSCREEN` | n | 5.3 离屏/扩展 blend（独立 Gate） |
| `CONFIG_OTOOL_CUBISM_RENDER_SIZE_*` | 640×360 | 实时渲染分辨率档位 |
| `CONFIG_OTOOL_CUBISM_RASTER_WORKERS` | 1 | 光栅 worker 数（B3/B6 决定） |
| `CONFIG_OTOOL_CUBISM_METRICS` | y | 指标采集 |

## 目录说明

```
research/   来源清单（reference_manifest.yml）与 G-LGL 审查单 —— 不进入固件
spec/       内部规格骨架（format/behavior/错误码/hard limits/测试向量 schema）
tools/      PC 端工具（corpus_tool 已可用；oracle_runner/diff_runner 等后续）
test/       host/fuzz/target 测试与向量清单（当前骨架）
```

## 端口注入

`otool_cubism_config_t` 注入 display / storage / stream / clock 四个端口
（见 `include/otool_cubism_port.h`）。组件不反向依赖板卡全局对象。

## 最小调用示例

```cpp
#include "otool_cubism_tool.h"
#include "otool_cubism_port.h"

static otool::cubism::otool_cubism_tool s_tool;

otool_cubism_config_t cfg = {};
cfg.display = &my_display_port;   // S1 起提供真实 display lease
cfg.storage = &my_storage_port;

ESP_ERROR_CHECK(s_tool.init(cfg));
ESP_ERROR_CHECK(s_tool.load_package("/sdcard/live2d/manifest.bin"));
ESP_ERROR_CHECK(s_tool.start(OTOOL_CUBISM_MODE_CLIP_PLAYER));
// ... 运行 ...
s_tool.stop();
s_tool.deinit();
```

## 许可与治理

- 自研 Core 属 **reference-assisted**（非严格 clean-room），每个研究来源必须
  在 `research/reference_manifest.yml` 登记并经审批；G-LGL 关闭前不写格式/行为代码。
- 本组件骨架不含任何 Live2D 代码。对外命名为 "otool 独立 moc3-compatible
  runtime"，不声称官方 Cubism Core / Live2D 背书。
- 完整 Gate 体系见 `docs/live2d_feasibility.md` §6.6。
