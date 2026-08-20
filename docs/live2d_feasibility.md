# otool_tab5_live2d — Live2D 上屏可行性报告

> 项目：M5Stack Tab5（ESP32-P4，720×1280 DSI 屏）运行 Live2D 角色动画
> 参考 SDK：`third_party/CubismSdk/CubismSdkForNative-5-r.5`（Native）与 `CubismSdkForWeb-5-r.5`（Web）
> 状态：可行性研究完成，待选型确认后进入实施

---

## 1. 结论摘要

| 问题 | 结论 |
|---|---|
| ESP32-P4 能跑 Live2D 吗？ | **能，但有条件**。实时渲染需要社区复刻 Core + 软渲染器，预计半分辨率 10~25 FPS；若接受预渲染播放则零风险 |
| 最大障碍 | Live2D **官方 Core 为闭源二进制**，官方仅提供 Windows/macOS/Linux/iOS/Android/HarmonyOS/RPi(实验) 版本，**无 RISC-V / ESP32 版本** |
| 可行路径 | ① 官方开源 Framework（纯 C++，可直接编入 ESP-IDF）+ ② 社区复刻 Core（API 兼容）+ ③ 自研软光栅渲染器 |
| 硬件匹配度 | ESP32-P4（双核 400MHz RISC-V + PSRAM + 硬件 JPEG 解码器 + 16MB Flash）是当前 ESP32 家族中最适合的平台；屏幕为 DSI 720×1280 |
| 先例 | 立创开源广场已有 ESP32-P4 桌宠类项目（如 PicoView 智能副屏）；ESP32-S3 社区有低分辨率 Live2D 桌宠 |

---

## 2. 硬件与软件现状

### 2.1 硬件（M5Stack Tab5）

- SoC：**ESP32-P4**，双核 RISC-V @ 400MHz，约 800 MIPS
- 内存：内置 SRAM + 板载 PSRAM（数十 MB 级，可用于帧缓冲与模型数据）
- 屏幕：MIPI DSI 720×1280（逻辑方向 1280×720，LVGL 经 PPA 90° 旋转）
- 存储：16MB QIO Flash @120MHz（HPM）
- 外设：硬件 **JPEG 解码器**、PPA（像素处理加速器，支持矩形缩放/旋转/镜像）、I2C 触摸

### 2.2 软件现状

- ESP-IDF v6.1-beta1（`C:\Espressif\frameworks\esp-idf-v6.1-beta1`），esp32p4 目标
- LVGL 9.5.0（`lvgl__lvgl` 托管组件），`otool_lvgl_idf_port` 提供 DSI+PPA 旋转、direct_mode 整屏直写
- 子模块组件：`otool_tab5_component`（硬件初始化）、`otool_dev_autodetect`
- 当前固件：白屏 + 居中 "hello onexs."（已验证可烧录运行，设备在 COM3）

### 2.3 参考 SDK（本机已有，`third_party/` 已被 git 忽略）

- `CubismSdkForNative-5-r.5/`：
  - `Core/` — 闭源核心库（仅官方支持平台的预编译 `.a`/`.lib` + 头文件 `Live2DCubismCore.h`）
  - `Framework/` — **开源** C++ 框架（Model / Motion / Physics / Effect / Math / Rendering / Id / Type / Utils），标准 CMake 静态库，无平台依赖
  - `Samples/` — D3D11/D3D9/Metal/OpenGL/Vulkan 示例 + 8 个官方测试模型（Haru、Hiyori、Mao、Mark、Natori、Ren、Rice、Wanko，含 moc3/motion3/physics3/贴图）
- `CubismSdkForWeb-5-r.5/` — Core 的 WebAssembly 版（不适用于本机原生渲染）
- `tools/` — 便携 CMake 4.0.3 / Ninja 1.12.1 / Git 2.44.0 + PowerShell 激活/构建脚本（`Activate-CubismNative.ps1`、`Build-CubismNativeD3D11.ps1`），用于 PC 端构建验证

---

## 3. Live2D 运行原理与移植拆解

Live2D 渲染一条链路，拆成三层后每层的移植难度差异很大：

```
.moc3/.motion3/.physics3/.cdi + 贴图
        │
        ▼
┌───────────────────────────┐
│  Core（闭源）              │  moc3 解析、参数→顶点形变（deformer）、
│  csmInitializeModel 等     │  绘制信息提取。官方只发二进制。
└───────────────────────────┘
        │  csmUpdateModel → 顶点/UV/索引/不透明度
        ▼
┌───────────────────────────┐
│  Framework（开源 C++）     │  模型装配、运动/物理/表情驱动、参数管理、
│                           │  CubismClippingManager（遮罩裁剪）、
│                           │  CubismRenderer 抽象基类（绘制接口）
└───────────────────────────┘
        │  Drawables 网格 + 纹理
        ▼
┌───────────────────────────┐
│  Renderer（平台相关）       │  官方：OpenGL/D3D/Metal/Vulkan
│                           │  本项目：自研软光栅（RGB565 帧缓冲）
└───────────────────────────┘
        │
        ▼
      DSI 屏幕
```

| 层 | 移植难度 | 说明 |
|---|---|---|
| Framework | ★☆☆ | 纯 C++、无 OS/GPU 依赖，直接作为 ESP-IDF 组件编译；仅需适配内存分配器（默认 malloc 即可） |
| Renderer | ★★☆ | 官方只给 GPU 渲染器；需按 `CubismRenderer` 接口实现软光栅：三角形遍历 + 双线性纹理采样 + 混合（Normal/Add/Multiply）+ 裁剪遮罩 |
| Core | ★★★ | 闭源无 RISC-V 版。需采用社区复刻（见 §4）或自研（解析 moc3 + 形变数学，Live2D 公开格式与算法文档） |

---

## 4. Core 替代方案对比

Core 对外 API 面很小（约 10 个函数：`csmInitializeModel`/`csmUpdateModel`/`csmReadCanvasInfo`/`csmGetDrawable*`/`csmGetParameter*` 等），这使"复刻"具备可行性。

| 方案 | 来源 | 语言 | 成熟度 | 适配 ESP-IDF 的工作量 |
|---|---|---|---|---|
| **PurismCore**（OpenL2D 组织） | 社区开源（relatedrepos/GitHub 可查，仓库名含 `_Old` 后缀，需确认当前版本） | C++ | 中 | 低：按 `Live2DCubismCore.h` 头文件实现符号即可 |
| **moc3-reader-re**（ShigemoriHakura） | 社区开源，逆向 Live2D 产物 | C/C++ | 中（偏解析器） | 中：需补全形变/绘制信息 API |
| **Mocari**（Eatgrapes） | 社区开源 | Rust | 低（experiment） | 高：需 Rust→IDF 集成（esp-rs），暂不推荐 |
| **Live2D-v2-Lua**（EasyLive2D） | 社区开源 | LuaJIT | 中 | 高：LuaJIT 在 RISC-V 支持有限，不推荐 |
| 官方实验构建（linux-arm64 / rpi） | 官方 | 二进制 | 高 | 不适用：架构不同（ARM vs RISC-V） |

**推荐**：以 **PurismCore（或同类 C/C++ 复刻）** 为第一候选，需做一次"API 兼容性 + 形变正确性"验证（即路线图 Phase 1 的 Spike）。

> 法律提示：moc3 格式与算法有公开文档，复刻解析/形变逻辑本身不侵权；但 Live2D SDK 及模型素材受其许可协议约束（个人/小规模免费，商用需授权），发布前需确认授权范围。

---

## 5. 推荐目标架构（ESP-IDF 组件划分）

```
otool_tab5_live2d/
├── main/                        # app：初始化、任务调度、触摸→参数映射
├── components/
│   ├── cubism_core_reimpl/      # 复刻 Core（.moc3 解析 + 形变更新）
│   ├── cubism_framework/        # 官方 Framework/src（原样编译，配 idf_component.yml）
│   ├── cubism_soft_renderer/    # 自研软光栅：CubismRenderer 派生类
│   │                            #   - RGB565 帧缓冲、双缓冲
│   │                            #   - 三角形光栅 + 双线性采样 + 混合模式
│   │                            #   - 裁剪遮罩（低分辨率离屏）
│   └── cubism_assets/           # 模型资源（从 Samples/Resources 挑选 + 打包到 flash）
└── third_party/CubismSdk/       # 参考资料（git 忽略）
```

### 5.1 渲染与显示路径

```
cubism_soft_renderer → RGB565 渲染缓冲（PSRAM，半分辨率 360×640）
        │
        ├─ 路线甲：直接 DSI 输出（绕过 LVGL，独享屏幕）—— 帧率最优
        └─ 路线乙：作为 LVGL image 对象显示 —— 便于与现有 UI 叠加/切换
```

- 依赖现有 `otool_tab5_component::lvgl_init()` 提供的显示通道；DSI 为全屏刷新，双缓冲 + 撕裂避免已由 `otool_lvgl_idf_port` 支持
- 触摸：`g_comp.lv_touch_indev()` 已有事件流，可直接映射为"拖拽/点击"参数（如注视点、眨眼、表情切换）

### 5.2 线程模型

| 任务 | 优先级 | 说明 |
|---|---|---|
| lvgl_task（现有） | 4 | 保持；渲染帧经 image 对象显示时由它刷新 |
| cubism_task（新增） | 5 | 模型更新（Core 形变，轻量）+ 软光栅（重）；按 60/30/15 FPS 节拍 |
| 触摸回调 | — | 更新参数缓冲（互斥或队列） |

### 5.3 内存预算（估算）

| 项目 | 大小 | 位置 |
|---|---|---|
| 渲染缓冲 360×640×2B × 2 | ~0.9 MB | PSRAM |
| 纹理 RGBA 2048×2048（或 RGB565） | 4~8 MB | PSRAM |
| moc3/motion3/physics3 + 贴图 | 数 MB | Flash（mmap 或读入 PSRAM） |
| Framework 对象 + 形变顶点 | < 1 MB | PSRAM |

### 5.4 性能预估与优化手段

基准线：官方模型（如 Haru/Hiyori）≈ 30~60 个 Drawable、数千三角形；角色约占屏高 60%。

| 手段 | 预期收益 | 备注 |
|---|---|---|
| 半分辨率渲染（360×640）后整屏缩放 | 像素量降到 1/4，**最有效** | PPA SRM 可做矩形缩放上屏 |
| 仅渲染角色包围盒区域 | 再省 30~50% 填充量 | 由 Drawable 包围盒求并集 |
| 双核并行：核 0 形变更新 / 核 1 光栅化 | ~1.5~1.8× | FreeRTOS 双任务 + 帧同步 |
| 纹理 RGB565 + 预乘 alpha | 带宽减半 | 上电时转换一次 |
| JPEG 纹理模型 + 硬件 JPEG 解码 | 加载加速 | 官方有 JPEG 版模型 |
| RISC-V SIMD/DSP 指令优化光栅内循环 | 1.2~2× | 手写汇编或 intrinsics，放最后 |
| 裁剪遮罩降为 1/4 离屏 | 遮罩开销大降 | 多数模型遮罩占比不高 |

**结论预期**：半分辨率 + 区域渲染 + 双核，**10~25 FPS 可实现**；进一步降至 240×427 可逼近 30 FPS。全分辨率（720×1280）软渲染**不建议**（<5 FPS）。

---

## 6. 备选路线（低风险 MVP）：预渲染播放

若先要"屏幕上出现 Live2D"，可在 PC 端用官方 SDK（本机已有 D3D11 构建脚本）把模型动作渲染成序列帧/视频，设备端用 PPA/JPEG 解码回放。

- 优点：零 Core 风险、帧率任意高、实现快（1~2 个工作日）
- 缺点：不可交互（除非预渲染多分支动画并按触摸切换）
- 定位：可作为实时移植完成前的过渡演示，或作为实时渲染的对照基准（视觉正确性）

---

## 7. 分阶段实施路线图

| 阶段 | 内容 | 交付物 / 验收 | 预估 |
|---|---|---|---|
| **P0 环境准备** | Framework 以 ESP-IDF 组件方式编译通过（链接复刻 Core 桩） | `idf.py build` 通过；`csmInitializeModel` 被调用 | 0.5 天 |
| **P1 Core Spike** | PC 端（MSVC/D3D11 或纯 CPU）：Framework + 复刻 Core 加载 Haru，导出顶点坐标与官方 Core 结果对比 | 形变误差 < 1px 视口坐标 | 1~2 天 |
| **P2 软渲染器** | 软光栅实现（三角形 + 双线性 + 混合 + 遮罩），PC 端先跑通输出 PNG 对照官方截图 | 视觉一致（半分辨率） | 2~3 天 |
| **P3 上板** | 组件移植到 ESP-IDF，帧缓冲经 DSI/LVGL 上屏；触摸映射基础参数（注视/眨眼） | 屏幕实时显示可交互角色 | 1~2 天 |
| **P4 优化** | 半分辨率、区域渲染、双核、纹理优化、SIMD | ≥15 FPS 稳定 | 2~3 天 |
| **P5 打磨** | 表情/动作切换、口型（可选音频）、背光联动、开机自启 | 桌宠产品化 | 按需 |

总计约 **1~2 周**（单人，P1 无重大意外的前提下）。

---

## 8. 风险与未决问题

| 风险 | 等级 | 缓解 |
|---|---|---|
| 复刻 Core 形变/绘制信息与官方不一致 | 中 | P1 Spike 用官方 Core（PC 上可跑）做数值对照；备选 moc3-reader-re / 自研 |
| 软渲染帧率不达标 | 中 | 分级降分辨率 + 区域渲染 + 双核；最差退化为预渲染方案 |
| 官方许可（Core 二进制分发、模型素材授权） | 低~中 | 内部使用无碍；发布前确认授权（个人/小规模免费） |
| PurismCore 等仓库活跃度/可用性 | 低 | 仓库不可用则自研（API 面小，有官方头文件与文档约束） |
| PSRAM 带宽与 DSI 全刷争用 | 低 | 双缓冲 + 撕裂避免已具备；必要时降帧率 |

---

## 9. 参考链接

- 官方平台支持：https://docs.live2d.com/zh-CHS/cubism-sdk-manual/platform/
- OpenL2D / PurismCore（Core 复刻候选）：https://relatedrepos.com/gh/OpenL2D/PurismCore_Old
- moc3-reader-re（逆向解析候选）：https://github.com/ShigemoriHakura/moc3-reader-re
- Mocari（纯 Rust Live2D 运行时，远期参考）：https://github.com/Eatgrapes/Mocari
- ESP32-P4 桌宠先例（PicoView 智能副屏）：https://oshwhub.com/yplam/project_aitmqhiu
- 嵌入式数据导出说明（moc3/motion3）：https://docs.live2d.com/en/cubism-editor-manual/export-moc3-motion3-files/
- Live2D-v2-Lua（纯 LuaJIT 引擎，备选参考）：https://github.com/EasyLive2D/Live2D-v2-Lua

---

*本报告基于 2026-08 检索信息与本地 SDK 5-r.5 实物分析；实施前建议复核各开源仓库的最新状态。*
