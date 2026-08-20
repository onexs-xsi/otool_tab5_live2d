# otool_cubism_tool

M5Stack Tab5（ESP32-P4）上的 Live2D/Cubism 领域组件 —— 唯一承载 Cubism 能力的
ESP-IDF 组件。`main` 只负责创建板级对象、注入端口并选择运行模式。

> 实现状态：**S0 组件骨架**（生命周期状态机 + 端口注入 + clip-only 构建）。
> 详细设计与阶段计划见 `docs/live2d_feasibility.md`。

## 运行模式

| 模式 | 对应路线 | 状态 |
|---|---|---|
| `CLIP_PLAYER` | D 预渲染片段播放 | Kconfig 默认开；播放器实现 S2 |
| `STREAM_CLIENT` | C 主机串流 | 默认关；实现 S2b |
| `REALTIME` | A/B 板端实时 | 默认关；**Core Gate 通过前开启会构建失败** |

## Kconfig

| 配置 | 默认 | 说明 |
|---|---|---|
| `CONFIG_OTOOL_CUBISM_ENABLE_CLIP_PLAYER` | y | 预渲染播放模式 |
| `CONFIG_OTOOL_CUBISM_ENABLE_STREAM_CLIENT` | n | 主机串流模式 |
| `CONFIG_OTOOL_CUBISM_ENABLE_REALTIME` | n | 板端实时（未实现，开启即 FATAL） |
| `CONFIG_OTOOL_CUBISM_CORE_BACKEND_*` | NONE | 实时模式 Core 后端（NONE/OFFICIAL/PURISM） |
| `CONFIG_OTOOL_CUBISM_RENDER_SIZE_*` | 640×360 | 实时渲染分辨率档位 |
| `CONFIG_OTOOL_CUBISM_RASTER_WORKERS` | 1 | 光栅 worker 数（B3/B6 决定） |
| `CONFIG_OTOOL_CUBISM_METRICS` | y | 指标采集 |

## 端口注入

`otool_cubism_config_t` 注入 display / storage / stream / clock 四个端口
（见 `include/otool_cubism_port.h`）。组件不反向依赖板卡全局对象。

## 最小调用示例

```cpp
#include "otool_cubism_tool.h"
#include "otool_cubism_port.h"

static otool::cubism::otool_cubism_tool s_tool;

// display/storage 端口由 main 提供（S1 起提供真实 display lease）
otool_cubism_config_t cfg = {};
cfg.display = &my_display_port;
cfg.storage = &my_storage_port;

ESP_ERROR_CHECK(s_tool.init(cfg));
ESP_ERROR_CHECK(s_tool.load_package("/sdcard/live2d/manifest.bin"));
ESP_ERROR_CHECK(s_tool.start(OTOOL_CUBISM_MODE_CLIP_PLAYER));

otool_cubism_input_event_t ev = {};
ev.kind = OTOOL_CUBISM_INPUT_TAP;
ev.sequence = 1;
ev.timestamp_us = esp_timer_get_time();
s_tool.submit_input(ev);

// ... 运行 ...

s_tool.stop();
s_tool.deinit();
```

## 许可提示

本组件骨架不含任何 Live2D 代码。引入官方 Framework/Core 或第三方 Core 复刻时，
须按可行性报告 §6.3/§11 完成许可确认与版本锁定（vendor/manifest.lock）。
