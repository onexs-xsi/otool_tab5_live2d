# otool_tab5_live2d — 自研 moc3/Core 与 otool_cubism_tool 组件化方案

> 目标设备：M5Stack Tab5（ESP32-P4、16 MB Flash、32 MB PSRAM、1280×720 MIPI-DSI）
>
> 本地参考：Cubism SDK for Native / Web 5-r.5、当前 ESP-IDF 工程与显示组件
>
> 复核日期：2026-08-21
>
> **项目定位（2026-08-21 修订）**：私有个人项目，**不开源、不上市、不发布**；
> 允许逆向分析等研究方式；本报告不设法律/合规门禁，只保留工程 Gate。
>
> 状态：**已完成 <code>components/otool_cubism_tool</code> 的 S0 骨架；技术主路线为组件内自研 moc3/Core 兼容运行时 + CPU 软光栅。自研 Core 尚未实现，REALTIME 仍由构建门禁关闭。**

---

## 1. 最终结论

### 1.1 一句话结论

本项目选择：在唯一组件 <code>components/otool_cubism_tool</code> 内实现一个**独立的、模型子集优先的 moc3/Core 兼容运行时**，再连接组件自有 CPU 软光栅器和 Tab5 显示链路。公共实现使用 <code>ot_core_*</code> 内部 API；首版不追求官方全部 ABI、全部 moc3 版本或全部 5.3 特性。

**可行性判定：研发立项为 Conditional Go，产品发布仍为 No-Go。** 只有第 6.6 节 G-FMT/G-BHV/G-SEC/G-TGT/G-RND/G-REL 全部关闭，才能把 REALTIME 纳入发布配置。

这是一项中长期研发，不是 2～5 周的普通移植。落地方式：

- 先冻结唯一生产模型与导出档，优先重导出为 **moc3 版本字节 5 / SDK 5.0 档**，禁用 5.3 离屏和扩展混合；
- 先交付安全解析器、不可变模型 IR 和目标模型所需的参数/形变功能，再逐项扩展；
- 用独立测试向量、模糊测试和 PC 对照程序验证行为，不能以“肉眼看起来相似”代替等价性；
- 在自研实时链完成前，继续保留 <code>CLIP_PLAYER</code> 作为设备可用版本和运行时故障降级；
- 研究来源（GitHub 参考项目、本地 SDK、逆向产物）固定 commit/hash 记录于 <code>research/reference_manifest.yml</code>，保证可复现。

### 1.2 路线选择

| 路线 | 独立运行 | 连续参数交互 | 当前成熟度 | 在本项目中的定位 |
|---|---|---:|---|---|
| **E. 自研 moc3/Core + 软渲染** | 是 | 是 | 研发期 | **已选主路线**；先做单一生产 profile，不承诺全版本/全 ABI |
| R. GitHub 参考实现 | 取决于项目 | 取决于项目 | 差异很大 | 结构参考、PC oracle、测试素材来源与逆向参考 |
| A. 官方 RISC-V Core + 软渲染 | 是 | 是 | 官方当前未公开支持 ESP/RISC-V | 若未来获得，可作备选，不再阻塞主路线 |
| C. 主机实时渲染 + JPEG 串流 | 否 | 是 | 中～高 | 可选完整效果方案；也是研发期间的联调手段 |
| D. 预渲染片段 + 交互状态机 | 是 | 离散状态 | 高 | 可交付安全网和 REALTIME 故障 fallback，不取代主路线 |

上述能力全部归入 <code>otool_cubism_tool</code>。业务层只面对一个公共 API；自研 Core、动画层、软光栅、JPEG 播放、串流、输入映射、显示提交和指标采集都留在组件内部。Web Core/asm.js 方案仍排除。

### 1.3 推荐执行顺序

1. 冻结生产模型 profile。若有工程源文件，优先导出为 moc3 版本字节 5，禁用 offscreen、扩展 blend，并固定 drawable/parameter/texture 上限。
2. 在组件内建立 <code>spec/</code>、<code>research/reference_manifest.yml</code>、<code>test/vectors/</code> 和 PC 端 oracle 协议；先形成可审计规格，不先堆固件代码。
3. 实现 bounds-checked moc3 parser → 不可变 IR → 独立 runtime arena；解析 Gate 通过后才实现参数绑定、插值、warp/rotation deformer、parts/artmesh。
4. 先使用组件内部 <code>ot_core_*</code> API。只有 Framework 复用确实需要时，才增加私有 <code>csm*</code> compatibility shim。
5. Core 行为 Gate 通过后再连接 CPU 软光栅并跑 Tab5 B3～B6；任何阶段失败都保持 CLIP/STREAM 可用，不把未验证 REALTIME 带入发布构建。

---

## 2. 证据与修正

后文使用三种标记：

- **已验证**：由当前仓库、本地 SDK、交叉编译或官方资料直接确认。
- **待实测**：必须在 Tab5 真机上测量，不能由规格表推断。
- **假设**：用于制定初始实验参数，不是性能承诺。

### 2.1 已验证的本地事实

| 项目 | 结果 |
|---|---|
| 硬件 | Tab5 官方规格为 ESP32-P4 双核最高 400 MHz、16 MB Flash、32 MB PSRAM、1280×720 DSI、microSD |
| 当前显示路径 | 物理 720×1280 RGB565；LVGL 逻辑 1280×720；PPA 做 90° 旋转 |
| 当前显示内存 | 2 个 DSI RGB565 帧缓冲约 **3.52 MiB**，另有 1 个 LVGL 全屏逻辑缓冲约 **1.76 MiB**，合计约 **5.27 MiB** |
| 当前构建模式 | **Debug / -Og**；现有配置不能用于推断最终渲染性能 |
| IDF 版本 | 实际构建为 **v6.1-beta1**；<code>main/idf_component.yml</code> 已约束 <code>>=6.1.0-beta1,<6.2.0</code>，lock 中 <code>6.1.0</code> 是组件管理器规范化表示。仍需在报告中记录实际 IDF commit/toolchain，且 beta 本身是回归风险 |
| Core API | 本地 5-r.5 头文件有 **55 个导出函数** |
| Framework 可编译性 | 排除官方 GPU 后端后，49 个 Framework C++ 源文件已用 ESP RISC-V GCC 15.2、<code>-O2 -fno-exceptions -fno-rtti</code> 静态交叉编译通过 |
| Purism Core 可编译性 | v1.0.1（历史固定提交 <code>166785bb…</code>）16 个 C 源文件可用 ESP RISC-V GCC 15.2、<code>-O2 -Werror</code> 编译通过；需显式设置 <code>PSM_HAS_STDINT=0</code>。这只证明能生成目标文件 |
| Purism Core 可移植问题 | 不加上述宏时，ESP 工具链因 <code>int32_t</code> 映射为 <code>long</code>、API 返回 <code>int*</code> 而在 <code>param.c</code> 编译失败；必须固定补丁/编译选项并回归 |
| 本地示例资源 | 8 个模型压缩文件总量约 0.7～4.9 MB/模型；moc3 约 0.1～0.9 MB；多数纹理为 2048×2048 |
| 本地 moc3 版本 | 本地 5-r.5 头文件定义版本字节 1～6；8 个样例实际包含字节 1、3、5、6（corpus_tool 实测：Haru/Natori/Wanko=1，Hiyori/Mark/Rice=3，Mao=5，Ren=6）。只支持 1～5 的解析器无法加载本地 Ren 样例 |
| Web Core | 本地 Web 5-r.5 提供内联 Emscripten/asm.js 的 <code>live2dcubismcore.js</code>，没有可直接交给嵌入式 WASM Runtime 的独立 <code>.wasm</code> 文件 |
| 目标组件现状 | S0 骨架已经存在：CMake、Kconfig、公共头文件、生命周期状态机和默认时钟端口已实现；CLIP/STREAM 产帧、自研 Core、Renderer 和 REALTIME 尚未实现 |

交叉编译通过只证明“源码能为 RISC-V 生成目标文件”，**不证明模型能正确加载、不证明数值等价、更不证明板端速度足够**。

### 2.2 原报告中必须撤回的判断

| 原判断 | 修正 |
|---|---|
| “Core API 面很小，约 10 个函数” | 本地头文件实际为 55 个导出函数；ABI 兼容、动态标志、遮罩、颜色、离屏绘制都要验证 |
| “moc3 格式与算法有公开文档，复刻本身不侵权” | 官方公开的是 Core API，不等于公开完整 moc3 格式/实现算法；本报告不给出任何法律结论（私有项目定位，合规限制已移除） |
| “PurismCore_Old 成熟度中、适配工作量低” | 当前项目已迁至 <code>SakuraMotion/PurismCore</code>；仓库很新且无 ESP32/RISC-V CI，只作为实验候选 |
| “Framework 直接编入 ESP-IDF 即可” | 基础源文件确实可交叉编译，但仍需统一封装进 <code>otool_cubism_tool</code>，并提供 PSRAM 对齐分配器、日志/文件系统策略和自定义 Renderer 工厂 |
| “RGBA 2048² 纹理 4～8 MB” | 单张 RGBA8888 2048² 是 **16 MiB**；RGBA4444 是 **8 MiB**。Live2D 纹理需要 alpha，不能简单改成无 alpha 的 RGB565 |
| “双核：一核形变、一核光栅可得 1.5～1.8×” | 同一帧光栅依赖 Core 更新后的顶点；直接并行存在依赖和数据竞争。应先更新，再按 tile 并行光栅，收益必须实测 |
| “半分辨率 10～25 FPS、全分辨率 <5 FPS” | 当前没有板端三角形基准，且构建仍为 <code>-Og</code>。这些数字无证据，改为验收门槛而不是预测 |
| “1～2 周完成” | 自研 Core 的生产模型子集按单名资深工程师估算约 3～6 个月；多版本、完整语义和兼容 ABI 通常应按 6～12 个月以上管理 |

---

## 3. 真正的技术边界

### 3.1 Core、Framework、Renderer 是三个独立问题

~~~text
model3.json / moc3 / motion3 / physics3 / textures
                         │
                         ▼
        Core：moc3 装载、参数到顶点/绘制状态
                         │
                         ▼
   Framework：动作、表情、物理、参数和模型生命周期
                         │
                         ▼
  Renderer：纹理三角形、alpha、混合、遮罩、离屏效果
                         │
                         ▼
  Display Presenter：PPA 缩放/旋转 → DSI 双缓冲 → VSYNC
~~~

- 官方 Core 的 C API 本身可移植，但官方公开下载中没有 RISC-V/ESP32 二进制。
- Framework 是源代码可见的 C++ 层，可交叉编译。
- ESP32-P4 的 PPA 能做矩形缩放、旋转、镜像、填充和图层混合，**不能光栅化带 UV 的三角网格**。
- 所以板端实时路线无论采用哪个 Core，都仍然需要 CPU 软光栅器。

### 3.2 MVP 必须冻结 moc3 版本和模型特性

“能读取 MOC3 magic”不等于兼容某个模型。版本字节改变的不只是文件布局，还可能引入新的运行语义。本地 5-r.5 头文件给出的版本对应关系为：

| 版本字节 | 导出版本范围 | MVP 策略 |
---:|---|---|
| 1 | 3.0.00～3.2.07 | 回归样例；生产模型需要时再支持 |
| 2 | 3.3.00～3.3.03 | 后续兼容 |
| 3 | 4.0.00～4.1.05 | 回归样例；生产模型需要时再支持 |
| 4 | 4.2.00～4.2.04 | 后续兼容 |
| **5** | **5.0.00～5.2.03** | **首选生产 profile** |
| 6 | 5.3.00+ | 单独里程碑；不得在只实现布局后宣称支持 |

本地样例分布为：Haru/Natori/Wanko＝字节 1，Hiyori/Mark/Rice＝字节 3，Mao＝字节 5，Ren＝字节 6。它们适合做多版本回归发现，但首版的产品承诺仍只针对获准的生产 profile。

第一版优先要求把自有生产模型重新导出为版本字节 5。若原始工程无法重导出，或模型必须使用版本字节 6 的 offscreen/扩展混合能力，项目经理必须重新批准范围、测试语料和工期，不能让 parser “尽量加载”。设备只接受离线校验并签名的模型包：

| 约束 | MVP 规则 |
|---|---|
| 画布 | 16:9；首选 640×360 渲染后放大 |
| 纹理 | 最多 2 张；离线缩至不超过 1024² |
| 纹理格式 | 首选预乘 alpha 的 RGBA4444；若边缘质量不足，再评估 RGB565 + A8 |
| 混合 | Normal / Add / Multiply |
| 遮罩 | 支持普通和反相 mask；数量与 atlas 尺寸由基准确定 |
| 5.3 离屏绘制 | MVP 禁止 |
| 扩展颜色/alpha blend mode | MVP 禁止 |
| moc3 版本 | 首版只允许 manifest 声明且离线验证过的字节 5；未知或不匹配直接拒绝 |
| 模型来源 | 固定、可信、随固件/SD 素材包发布；不接受用户上传任意 moc3 |
| 超限行为 | 离线打包器直接报错，设备端拒绝加载，不能静默降级为错误画面 |

应实现一个与固件 parser 共享 schema、但不共享不安全快捷路径的 PC 端 <code>asset_validator</code>，输出 Drawable、顶点、三角形、deformer 深度、keyform 数、mask、纹理、blend mode、offscreen、moc 版本和预计 RAM；只有通过 profile 且包签名/哈希正确的资源才允许上板。

### 3.3 Web SDK 不是捷径

本地 Web Core 是面向 JavaScript/WebGL 宿主的 asm.js 产物，且可再分发文件清单只列 JS/类型声明文件。即便引入 QuickJS 等运行时，也会同时带来：

- asm.js 数值计算和 JS↔C/帧缓冲桥接开销；
- 额外堆内存、垃圾回收和启动时间；
- WebGL 仍不存在，Renderer 仍要改写。

因此不把“Web Core + WASM/JS Runtime”列为可交付路线。

### 3.4 研究与代码来源策略

本项目为私有项目（不开源、不上市、不发布），允许逆向分析等研究方式；研究不设审批门禁，但保留工程可复现性要求：

| 来源类别 | 使用方式 |
|---|---|
| 本地 Live2D SDK（third_party/CubismSdk） | 格式逆向的直接对象；官方 Core 作为行为 oracle 对照 |
| GitHub 参考实现（PurismCore/Mocari/ayagami 等） | 结构参考、算法交叉检查、PC oracle；不直接作为固件 backend |
| 逆向分析产物 | 允许；作为实现依据时记录来源 |
| 测试向量/输出 | 允许；固定 hash 便于复现 |

- 所有研究来源固定 URL / commit / hash 于 <code>research/reference_manifest.yml</code>（工程记录，非合规门禁）。
- 对外命名仍使用“<code>otool</code> 独立 moc3-compatible runtime”；不得声称它是官方 Cubism Core 或获得 Live2D 背书。

---

## 4. 统一 ESP-IDF 组件：components/otool_cubism_tool

### 4.1 组件定位与边界

<code>otool_cubism_tool</code> 是工程中唯一承载 Live2D/Cubism 领域能力的 ESP-IDF 组件。<code>main</code> 只负责创建板级对象、注入端口并选择运行模式，不再直接调用 Cubism Core、Framework、JPEG、PPA 或播放器内部接口。

组件支持三个可独立裁剪、也可同时编入固件的运行能力：

| 运行模式 | 对应路线 | 固件内 Core | 组件内部路径 | 默认 |
|---|---:|---:|---|---:|
| <code>CLIP_PLAYER</code> | D | 不需要 | 素材包 → JPEG → presenter | **开启** |
| <code>STREAM_CLIENT</code> | C | 不需要 | 帧协议 → JPEG → presenter | 可选 |
| <code>REALTIME</code> | E | 组件内 <code>SELF_CORE</code> | animation → self Core → 软光栅 → presenter | Gate 通过后开启 |

统一集成不等于把 PC 工具编译进固件。离线验证、打包和对照工具归属该组件，但放在 <code>tools/</code> 下并从 <code>idf_component_register</code> 的源码列表中排除。

强制边界：

- 公共头文件不得暴露 <code>csmModel*</code>、Framework 类、PPA client、LVGL 对象或第三方容器类型。
- self Core、可选 Framework adapter、Renderer 和第三方头文件只能由组件私有源码包含。
- 业务代码不得绕过门面调用 Core，也不得直接写 DSI framebuffer。
- <code>CLIP_PLAYER</code> 必须能在完全不链接 Core/Framework 的配置下独立构建。
- 即使启用 <code>REALTIME</code>，<code>CLIP_PLAYER</code> 仍作为加载失败、运行超时或模型不兼容时的安全降级。
- 固件 self Core 首先暴露组件私有 <code>ot_core_*</code> typed API；不为了宣称兼容而过早复制完整 <code>csm*</code> ABI。
- PurismCore、Mocari 等研究候选不作为发布 Kconfig backend；若做本地比较，只能进入 host tools 或隔离的实验构建。

### 4.2 建议目录结构

~~~text
components/otool_cubism_tool/
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── README.md
├── research/                         # 不进入固件
│   └── reference_manifest.yml        # URL/commit/hash/来源记录（工程可复现）
├── spec/                             # 版本化的内部规格
│   ├── format/                       # 目标 moc3 版本的字段与校验规则
│   ├── behavior/                     # 参数、插值、形变、flags 行为契约
│   └── test_vector_schema.md         # oracle 输入/输出格式和误差规则
├── include/
│   ├── otool_cubism_tool.h           # 唯一业务门面
│   ├── otool_cubism_types.h          # 配置、事件、状态、指标
│   └── otool_cubism_port.h           # display/storage/stream 端口
├── src/
│   ├── otool_cubism_tool.cpp         # 生命周期与状态机
│   ├── runtime/                      # coordinator、队列、fallback
│   ├── core/
│   │   ├── core_api.hpp              # 组件私有 ot_core_* typed API
│   │   ├── self/
│   │   │   ├── moc3_reader.cpp       # 只读、bounds-checked reader
│   │   │   ├── moc3_validate.cpp     # 上限/引用/DAG/溢出验证
│   │   │   ├── moc3_ir.cpp           # 文件布局 → 不可变 IR
│   │   │   ├── arena.cpp             # 独立 runtime arena
│   │   │   ├── parameter.cpp         # clamp/repeat/key search
│   │   │   ├── interpolate.cpp       # 权重、插值与外推
│   │   │   ├── deformer_graph.cpp    # 拓扑排序和 cycle 拒绝
│   │   │   ├── deformer_warp.cpp
│   │   │   ├── deformer_rotation.cpp
│   │   │   ├── blendshape.cpp
│   │   │   ├── part.cpp
│   │   │   ├── artmesh.cpp
│   │   │   ├── glue.cpp
│   │   │   ├── draw_order.cpp
│   │   │   ├── dynamic_flags.cpp
│   │   │   ├── update.cpp
│   │   │   └── offscreen_v6.cpp      # MVP 不编译，单独 Gate
│   │   └── compat/
│   │       └── csm_compat.cpp        # 可选、私有、行为稳定后再实现
│   ├── animation/
│   │   ├── self/                     # 默认：目标 profile 的 motion/expression/pose/physics
│   │   └── framework_adapter/        # 可选：Framework + 私有 csm shim
│   ├── assets/                       # manifest、CRC、profile、缓存
│   ├── renderer/                     # tile raster、blend、mask atlas
│   ├── presenter/                    # JPEG、PPA、DSI、VSYNC
│   ├── player/                       # 预渲染片段与交互状态机
│   ├── stream/                       # 帧协议、PTS、CRC、丢旧帧
│   ├── input/                        # touch/audio → 统一事件
│   ├── metrics/                      # P50/P95/P99、堆、丢帧
│   └── platform/                     # ESP-IDF allocator、clock、task
├── vendor/
│   ├── manifest.lock                 # 每个实际依赖的版本、补丁、SHA-256
│   └── live2d_framework/             # 仅在确需复用时导入
├── resources/
│   └── fallback/                     # 最小 Flash 内置片段
├── tools/                            # PC 端，不进入固件
│   ├── oracle_runner/                # 多实现进程隔离、统一 JSONL 协议
│   ├── spec_probe/                   # 生成行为规格/向量
│   ├── diff_runner/                  # static exact / float tolerance 比较
│   ├── corpus_tool/                  # 资产清单、去重、版本/feature 统计（已实现）
│   ├── asset_validator/
│   ├── asset_packer/
│   └── clip_packer/
└── test/
    ├── vectors/                      # 向量清单（模型不入库，按 hash 取用）
    ├── host/                         # parser、算法、property、差分测试
    ├── fuzz/                         # parser/update seed、dictionary、harness
    └── target/                       # B0～B6 与故障注入
~~~

当前 SDK 位于仓库级 <code>third_party/CubismSdk</code>，Spike 阶段通过明确的 <code>CUBISM_SDK_ROOT</code> CMake cache 变量引用，禁止在组件中写死本机绝对路径。发布版 self Core 不链接官方 Core 或第三方重实现。若动画层确需复用官方 Framework，用受控导入脚本写入 <code>vendor/live2d_framework</code> 与 <code>manifest.lock</code>；否则实现组件自有的目标子集。禁止 configure 阶段联网下载、跟随分支或静默替换版本。

### 4.3 内部模块职责

| 内部模块 | 唯一职责 | 允许依赖 |
|---|---|---|
| facade/runtime | 生命周期、状态迁移、错误恢复、模式切换 | 其余内部接口 |
| core/self | 安全读取目标 moc3、维护 runtime arena、计算 drawable 状态 | spec、assets、platform allocator |
| core/compat | 可选的私有 <code>csm*</code> shim；不得成为公共 API | core/self |
| animation/self | 默认 animation backend；目标 profile 的 motion、expression、pose、physics | core/self、assets |
| animation/framework_adapter | 可选的官方 Framework 封装，不得越过 facade | core/compat、assets、受控 vendor |
| assets | 可信包、manifest/profile/CRC、按需读取 | storage port |
| renderer | 纹理三角形、blend、mask、tile worker | draw snapshot、assets |
| presenter | JPEG decode、PPA scale/rotate、VSYNC present | display port、IDF driver |
| player | 预渲染片段调度、循环点、交互跳转 | assets、presenter |
| stream | 帧拆包、PTS、CRC、队列深度 1 | stream port、presenter |
| input | 触摸/音量/命令归一化，不直接改 Core | runtime queue |
| metrics | 分阶段耗时、内存、丢帧、错误计数 | 只读观测点 |

内部依赖必须保持单向；例如 presenter 不得反调 player，renderer 不得读取 LVGL 全局状态，self Core 不得自行打开文件或访问显示。parser 只把字节变成经过验证的不可变 IR；update 不再解释原始 offset。

### 4.4 公共 API 与生命周期

建议公共 C++ API 保持小而稳定，语义先冻结，具体字段在实现时微调：

~~~cpp
namespace otool::cubism {

enum class mode {
    clip_player,
    stream_client,
    realtime,
};

class otool_cubism_tool {
public:
    esp_err_t init(const config& cfg);
    esp_err_t load_package(const char* manifest_path);
    esp_err_t start(mode run_mode);
    esp_err_t submit_input(const input_event& event);
    esp_err_t set_parameter(const parameter_update& update);
    esp_err_t get_status(status* out) const;
    esp_err_t get_metrics(metrics* out) const;
    esp_err_t stop();
    esp_err_t deinit();
};

} // namespace otool::cubism
~~~

API 规则：

- <code>init()</code> 只分配基础资源并校验端口，不接管显示。
- <code>load_package()</code> 先验证 manifest、版本、CRC、模型 profile 与预算，再提交新资源；失败时旧资源保持可用。
- <code>start()</code> 取得显示独占租约后才创建/唤醒工作任务；重复调用返回明确的状态错误。
- <code>submit_input()</code> 是所有模式统一的非阻塞入口；队列满时合并连续坐标并保留最新状态。
- 输入事件携带单调时钟时间戳；组件丢弃已过期坐标，离散点击事件则按 sequence 去重。
- <code>set_parameter()</code> 仅在实时模式有效；其他模式返回 <code>ESP_ERR_NOT_SUPPORTED</code>，不能假装成功。
- <code>get_status()</code> 返回当前状态、活动模式、素材版本和最后错误，不暴露内部对象。
- <code>stop()</code> 必须先停产帧，以有界超时等待或取消 PPA/JPEG，确认硬件不再访问 framebuffer 后再释放显示租约。若无法确认静止，则进入 ERROR 并保持租约，禁止 LVGL 恢复写屏。
- <code>deinit()</code> 必须幂等；任何部分初始化失败都能沿同一路径回滚。
- 不向调用者返回内部模型或 framebuffer 的长期裸指针。

状态迁移固定为：

| 当前状态 | 操作/事件 | 下一状态 | 约束 |
|---|---|---|---|
| UNINITIALIZED | <code>init</code> | READY | 只建立基础资源，不取显示租约 |
| READY / LOADED | <code>load_package</code> | LOADED | 完整校验后原子替换；失败保持原状态和旧包 |
| LOADED | <code>start</code> | RUNNING | 先取得显示租约；失败仍为 LOADED |
| RUNNING / CLIP_FALLBACK | <code>stop</code> | LOADED | 确认产帧和 DMA 静止后才归还租约 |
| READY / LOADED | <code>deinit</code> | UNINITIALIZED | 幂等释放全部组件资源 |
| RUNNING | 可恢复故障 | CLIP_FALLBACK | 仅在 Flash fallback 已校验且 presenter 可用时 |
| 任意活动状态 | 不可恢复故障 | ERROR | 保留必要所有权，禁止继续提交帧 |
| ERROR | <code>stop/deinit</code> | READY 或 UNINITIALIZED | 只有硬件静止后才能恢复 LVGL |

<code>deinit()</code> 只能从非 RUNNING 状态执行；RUNNING 必须先 <code>stop()</code>。<code>main</code> 只持有一个 <code>otool_cubism_tool</code> 实例。模式切换必须经过 <code>stop → load（如需要）→ start</code>，不允许业务层直接销毁内部任务。

### 4.5 端口注入与组件依赖

为避免 <code>otool_cubism_tool</code> 反向依赖全局 <code>g_comp</code> 或绑定某一块板卡，<code>config</code> 注入以下能力：

| 端口 | 最小语义 |
|---|---|
| display | 获取/释放独占租约、取得 inactive framebuffer、提交并等待 VSYNC、查询物理格式与尺寸 |
| storage | VFS 根路径或受界限的 open/read/seek/close；区分 Flash fallback 与 SD 素材 |
| stream | 非阻塞读写、连接状态、取消等待；协议解析仍在组件内部 |
| clock | 单调微秒时钟；默认使用 <code>esp_timer_get_time()</code> |

依赖方向应为：

~~~text
main ──> otool_tab5_component
  └────> otool_cubism_tool ──> ESP-IDF jpeg/ppa/lcd/timer
               │
               └─ 只通过注入端口使用显示、存储和传输
~~~

因此 <code>main/CMakeLists.txt</code> 只需在 <code>REQUIRES</code> 中新增 <code>otool_cubism_tool</code>。组件公共头文件若只暴露 <code>esp_err_t</code> 和自有 POD 类型，则公开依赖保持最小；<code>esp_driver_jpeg</code>、<code>esp_driver_ppa</code>、<code>esp_lcd</code>、<code>esp_timer</code>、<code>fatfs</code> 等放入 <code>PRIV_REQUIRES</code>，最终名称以固定后的 ESP-IDF 版本为准。

### 4.6 CMake 与 Kconfig 约束

| 配置 | 规则 |
|---|---|
| <code>CONFIG_OTOOL_CUBISM_ENABLE_CLIP_PLAYER</code> | 默认开启 |
| <code>CONFIG_OTOOL_CUBISM_ENABLE_STREAM_CLIENT</code> | 默认关闭 |
| <code>CONFIG_OTOOL_CUBISM_ENABLE_REALTIME</code> | 默认关闭，Gate 通过后才允许发布配置开启 |
| <code>CONFIG_OTOOL_CUBISM_CORE_BACKEND_*</code> | NONE/SELF 二选一；REALTIME 必须为 SELF |
| <code>CONFIG_OTOOL_CUBISM_MOC_PROFILE_V5</code> | 首版唯一可发布 profile；与包 manifest 双向校验 |
| <code>CONFIG_OTOOL_CUBISM_ANIMATION_BACKEND_*</code> | SELF/APPROVED_FRAMEWORK 二选一；默认 SELF |
| <code>CONFIG_OTOOL_CUBISM_ENABLE_CSM_COMPAT</code> | 默认关闭；只在已启用 Framework adapter 时开启 |
| <code>CONFIG_OTOOL_CUBISM_ENABLE_V6_OFFSCREEN</code> | 默认关闭；独立版本/渲染 Gate 通过后才能开启 |
| <code>CONFIG_OTOOL_CUBISM_RENDER_SIZE_*</code> | 640×360 / 512×288 / 320×180 三选一 |
| <code>CONFIG_OTOOL_CUBISM_RASTER_WORKERS</code> | 1 或 2，由 B3/B6 决定 |
| <code>CONFIG_OTOOL_CUBISM_METRICS</code> | 开发默认开启，发布保留低成本计数 |

<code>CMakeLists.txt</code> 按 Kconfig 追加源文件，而不是把所有 backend 编译后再在运行时选择。配置阶段必须执行以下检查：

- 启用 REALTIME 却未选择 SELF，或 SELF 尚未生成通过 Gate 的 feature manifest：直接 <code>FATAL_ERROR</code>。
- 固件源码列表发现 official/purism/mocari backend、未知 vendor Core 或未锁定依赖：直接失败。
- 选择 APPROVED_FRAMEWORK 却缺少固定 manifest，或未开启所需 <code>csm</code> shim：直接失败；SELF 模式不得链接 Framework 符号。
- 包声明的 moc 版本/feature 超出编译 profile：在任何大块内存分配前拒绝加载。
- clip-only 构建不得出现 Core/Framework 未解析符号。
- host tools、测试模型和官方示例素材不得进入固件镜像。
- 仅 <code>resources/fallback</code> 中通过哈希校验的最小片段可用 <code>EMBED_FILES</code> 进入 Flash。
- 组件私有 C++ 统一使用 <code>-fno-exceptions -fno-rtti</code>；不要修改整个工程的编译语义。

CI 至少构建 <code>clip-only</code>、<code>clip+stream</code>、<code>realtime-self-v5</code>，并保存 <code>idf.py size-components</code> 结果。<code>realtime-self-v6</code> 只在相应 Gate 通过后加入；PC oracle 和研究依赖必须是另一套构建图。

### 4.7 三条运行路径汇合到同一 presenter

~~~text
CLIP_PLAYER：   manifest/SD → clip scheduler → JPEG ─────────┐
STREAM_CLIENT： transport → frame parser   → JPEG ──────────┼→ presenter
REALTIME：      model → animation/self Core → soft renderer ┘
                                                        │
                                                        ▼
                                      PPA scale/rotate → DSI → VSYNC
~~~

实时路径内部为：

~~~text
参数/动作/物理
      │
      ▼
self Core 单线程更新
      │  生成组件自有、只读 draw snapshot
      ▼
按 tile 分发三角形 ── CPU0 / CPU1 光栅 worker
      │
      ▼
640×360 RGB565 场景缓冲
      │
      ▼
PPA：2× 缩放 + 90° 旋转
      │
      ▼
未显示的 720×1280 DSI framebuffer
      │
      ▼
VSYNC 交换
~~~

PPA 只负责最后一步的矩形像素处理。首选分辨率档位：

| 档位 | PPA 放大 | 用途 |
|---|---:|---|
| 640×360 | 2× | 首选质量档 |
| 512×288 | 2.5× | 中档；比例与 PPA 1/16 缩放步进精确匹配 |
| 320×180 | 4× | 保底档 |

不要沿用“240×427”这类方向和比例不一致的尺寸。

### 4.8 与 LVGL 的显示所有权

当前 LVGL 任务固定在 CPU1，并持有一个 1280×720 逻辑全屏缓冲；DSI 端还持有两个帧缓冲。第一版统一采用 **显示独占租约**：

1. <code>otool_cubism_tool::start()</code> 通过 display port 请求独占租约。
2. 租约实现必须阻止新的 LVGL flush，等待正在进行的 flush/PPA/VSYNC 完成，再交出 framebuffer。
3. 运行期间只有组件内部 presenter 能提交 framebuffer；简单状态图标在场景缓冲中合成。
4. <code>stop()</code>、错误回滚和 <code>deinit()</code> 都必须归还租约并恢复 LVGL。

当前 <code>otool_lvgl_port_stop()</code> 只停止 tick timer，**不足以证明 LVGL task 和 flush 已静止**，不能直接用作租约实现。应先在 <code>otool_lvgl_idf_port</code> 增加成对的 display suspend/resume 或 acquire/release API，并让 suspend 等待 in-flight flush 清零。这个板级仲裁 API 不属于 Live2D 业务，但 <code>otool_cubism_tool</code> 必须把它作为接管显示的前置条件。

不要让 LVGL 与 Cubism 同时写同一个 framebuffer。后续确需叠加复杂 UI，再设计单一所有者的 compositor；不能让两个渲染任务只靠普通互斥锁竞争显示。

### 4.9 软光栅实现要点

- 16.16 或 24.8 定点 edge function，采用一致的 top-left fill rule。
- Cubism 是 2D 网格，UV 可做仿射插值，不需要透视校正。
- 纹理使用预乘 alpha；先实现 nearest 作为性能基线，再启用定点 bilinear。
- 完整实现 Drawable opacity、model color、multiply/screen color、culling、draw order。
- mask 使用 A8 atlas；按 clipping context 复用，不为每个 Drawable 分配全屏 mask。
- 只清理 damage rectangle；但 DSI 仍是整帧扫描，dirty region 只减少 CPU 填充，不会减少面板扫描带宽。
- 先按 16×16 或 32×16 tile 并行；Core 更新完成后再启动 worker。
- worker 只读 draw snapshot、索引、UV 和纹理，禁止与下一次 Core 更新并发读写同一模型。
- 每帧队列深度固定为 1：来不及就丢旧帧，不能堆积延迟。

### 4.10 线程建议

| 阶段 | 执行方式 | 原因 |
|---|---|---|
| runtime coordinator | 单一状态机任务 | 集中处理模式、资源和错误恢复 |
| 输入/参数采样 | 短临界区或有界队列 | 不直接操作 Core 内存 |
| Motion/Physics/self Core update | 单线程 | 保证模型状态一致 |
| draw snapshot | 单线程复制必要指针/标量，或在更新暂停期只读 | 划定生命周期 |
| tile raster | 1 或 2 worker A/B 实测 | 双核收益受 PSRAM 带宽限制 |
| JPEG submit | 单一有界队列 | clip/stream 共用硬件 codec，避免抢占 |
| PPA/VSYNC present | 组件内唯一 presenter | 防止 framebuffer 所有权冲突 |

所有任务句柄、队列、buffer 和硬件 client 都由 <code>otool_cubism_tool</code> 创建并销毁，业务层不得持有。原报告提出的“CPU0 更新下一帧、CPU1 渲染当前帧”只有在复制两份完整可渲染状态后才安全，MVP 不采用。

---

## 5. 内存方案

### 5.1 当前固定开销

| 项目 | 计算 | 约占 |
|---|---:|---:|
| DSI framebuffer ×2 | 720×1280×2 B×2 | 3.52 MiB |
| LVGL 逻辑 framebuffer ×1 | 1280×720×2 B | 1.76 MiB |
| 当前显示合计 |  | **5.27 MiB** |

<code>otool_cubism_tool</code> 取得显示独占租约后，若租约实现能安全释放并在归还时重建 LVGL 逻辑缓冲，可回收约 1.76 MiB；在完成反复 acquire/release 测试前，内存预算不得预支这部分空间。

### 5.2 实时模式建议预算

| 项目 | 初始预算 | 说明 |
|---|---:|---|
| 640×360 RGB565 场景缓冲 ×2 | 0.88 MiB | 一块渲染、一块交给 PPA；若同步足够可降为一块 |
| 1024² RGBA4444 纹理 ×2 | 4.00 MiB | 保留 alpha；不要使用无 alpha 的纯 RGB565 |
| 512² A8 mask atlas | 0.25 MiB | 数量按模型 profile 限制 |
| moc3 不可变 blob | 0.1～0.9 MiB（示例范围） | 只读、带长度；所有 offset 经 reader 检查，不能原地写入运行时指针 |
| 已验证 IR/索引 | **待实测** | 只保存目标 profile 所需的 typed view、拓扑序和预计算索引 |
| self Core runtime arena | **待实测** | 参数、part、deformer、drawable、flags 分区；由 <code>ot_core_query_memory()</code> 统一计算 |
| Animation/Motion/Physics | **待实测** | JSON、容器和动作切换可能造成峰值及碎片；首版按目标资产限量 |
| FreeRTOS 栈、队列、指标 | 待实测 | 栈优先放内部 RAM |
| 安全余量 | 至少 6 MiB 或总 PSRAM 的 20%，取较大者 | 用于碎片、峰值和恢复 |

<code>CLIP_PLAYER</code> 和 <code>STREAM_CLIENT</code> 不应分配 moc/model、纹理和 mask atlas，只保留有界 JPEG 输入缓冲、RGB565 场景缓冲及 presenter 资源。CMake 未启用 REALTIME 时，不得为了“将来可能使用”而静态预留实时路径内存。

禁止把 2048² PNG 的文件大小当作运行时纹理大小：

- 2048² RGBA8888：16 MiB/张；
- 2048² RGBA4444：8 MiB/张；
- 两张 RGBA8888 已经达到整机 32 MiB PSRAM，尚未计显示和模型。

### 5.3 分配策略

- 在组件 <code>platform/</code> 中实现统一 allocator/arena：大于阈值的模型、IR、纹理和场景缓冲放 PSRAM，小型控制块和任务栈留内部 RAM。只有复用官方 Framework 时才额外提供其 allocator adapter。
- <code>ot_core_query_memory()</code> 必须使用 checked add/multiply 算出完整布局，随后一次分配、一次初始化；update 热路径不得按帧 malloc/free。
- 所有 PPA、JPEG 和 self Core buffer 使用明确的地址/大小对齐；不要假定普通 <code>malloc</code> 或未对齐文件映射足够。
- 启动、模型加载、动作切换和运行 30 分钟后分别记录：
  - 内部 RAM / PSRAM free、minimum free、largest free block；
  - moc blob、validated IR、runtime arena 的实际字节和 high-water mark；
  - 每类纹理/mask/framebuffer 实际字节数。
- 运行时不解析/保留原始 PNG；PC 端转换为设备格式，可选 LZ4 压缩，启动时一次解压到 PSRAM。
- motion/physics 先限制数量并按需加载；若 JSON 峰值明显，再引入离线二进制格式。
- 卸载顺序固定为：停止 update → 等待 renderer snapshot 归还 → 清 runtime arena → 清 IR → 释放 blob；禁止 renderer 持有跨代裸指针。
- 连续执行至少 100 次 <code>init → load → start → stop → deinit</code>，确认内部 RAM、PSRAM、largest free block、PPA/JPEG client 和任务数量回到基线。

---

## 6. 自研 moc3/Core 的实现与验证方案

### 6.1 先定义组件内部契约，不先追完整官方 ABI

self Core 的产品职责是“可信模型包 + 参数状态 → 可渲染 draw snapshot”。它不读文件、不解 PNG、不跑 motion/physics、不碰 framebuffer，也不在 update 热路径分配内存。首版内部接口建议为：

~~~c
ot_core_result_t ot_core_inspect(
    ot_core_bytes_t moc, const ot_core_limits_t *limits,
    ot_core_model_info_t *out_info, ot_core_error_t *out_error);

ot_core_result_t ot_core_query_memory(
    const ot_core_model_info_t *info, ot_core_memory_plan_t *out_plan);

ot_core_result_t ot_core_create(
    ot_core_bytes_t moc, ot_core_mut_bytes_t arena,
    const ot_core_create_options_t *options, ot_core_model_t **out_model);

ot_core_result_t ot_core_set_parameters(
    ot_core_model_t *model, const ot_core_parameter_update_t *updates, size_t count);

ot_core_result_t ot_core_update(ot_core_model_t *model);
ot_core_result_t ot_core_get_draw_snapshot(
    const ot_core_model_t *model, ot_core_draw_snapshot_t *out_snapshot);
ot_core_result_t ot_core_reset_dynamic_flags(ot_core_model_t *model);
~~~

接口约束：

- 核心实现使用可同时在 host/ESP 构建的受限 C++17：<code>-fno-exceptions -fno-rtti</code>，不依赖 iostream、文件系统或隐式分配容器；边界只使用固定宽度整数和 POD/span-like view。
- 所有输入都携带长度；没有未绑定长度的裸 <code>void*</code>。
- <code>inspect/query_memory/create</code> 使用同一份经过版本化的 limits；分配大小必须可重复计算。
- moc blob 在模型生命周期内只读；文件 offset 永远不改写为指针。
- IR 与 mutable runtime 分离；renderer 只能读取有代际号和有效期的 snapshot。
- 相同 blob、初始值和参数序列必须产生确定性输出；错误必须包含稳定 error code、section 和 byte offset。
- <code>csm*</code> 兼容层只是可选 adapter。先证明 <code>ot_core_*</code> 行为，再按 APPROVED_FRAMEWORK 的实际调用集实现 shim；不以本地头文件的约 55 个符号作为首版完成条件。默认 SELF_ANIMATION 完全不需要该 shim。

### 6.2 安全 parser 与不可变 IR

moc3 是二进制 offset/count 图。即使产品暂时只加载随包发布的可信模型，也必须按不可信输入设计 parser，因为坏 SD、截断下载、版本混用和工具链缺陷同样会产生畸形数据。

加载流程固定为：

~~~text
signed package
      │  manifest/version/size/hash/profile
      ▼
bounded byte reader
      │  magic/endian/version/table/count/offset checked arithmetic
      ▼
structural validator
      │  range/alignment/overlap/reference/cardinality/DAG/budget
      ▼
immutable typed IR + precomputed topology
      │
      └──> separately allocated mutable runtime arena
~~~

最低校验集：

- 只接受 manifest 白名单中的版本和 endian；MVP 遇到版本字节 6、未知 flag 或非目标 profile 立即拒绝。
- 每个 <code>offset + count × element_size</code> 都使用 checked arithmetic；在解引用、分配和类型转换之前验证。
- 所有 section 必须位于 blob 内，满足该字段要求的对齐；不允许意外重叠、指向 header/table 或利用 wraparound 回到文件内。
- count 必须同时满足格式关系和产品 hard limit，例如 parameters、deformers、drawables、vertices、indices、masks、keyforms 与总运行内存上限。
- 所有 index、parent、mask、binding 和 keyform 引用做 referential-integrity 检查；deformer/part 依赖图必须可拓扑排序，并限制最大深度。
- 浮点输入拒绝 NaN/Inf；需要有序的 key 序列必须验证单调性和重复键规则。
- ID/字符串必须有界并转换成组件自有 ID 表，运行时不得在 blob 外查找终止符。
- validator 完成前不建立 runtime pointer、不启动 update、不分配纹理；失败路径释放所有临时资源且保持旧模型可用。

PC validator 和 ESP parser 应共享字段/schema 定义与测试向量，但分别保留实现边界，避免一个相同 bug 同时“验证”并“解析”成功。对生产包再叠加 manifest major version、总大小、逐文件 SHA-256/CRC、产品签名和 anti-rollback 规则；CRC 只用于误码检测，不能代替签名。

### 6.3 按可观察行为分层实现

每一层必须有独立输入/输出向量并通过 Gate，才能进入下一层：

| 层 | 实现内容 | 可观察输出 |
|---|---|---|
| C0 Header/Profile | magic、version、endian、canvas、计数与预算 | 版本、canvas、拒绝原因、memory plan |
| C1 Static Model | IDs、UV、indices、texture/mask/parent/constant flags | 所有静态数组精确匹配 |
| C2 Parameters | default/min/max、clamp/repeat、key search、binding 组合 | key 区间、权重、参数 runtime 值 |
| C3 Interpolation | 多参数 keyform 权重、边界和外推行为 | 规范化权重与 keyform 混合结果 |
| C4 Deformer Graph | warp、rotation、反射、scale、parent 链、拓扑更新 | 每级 deformer 输出点和 opacity |
| C5 Drawable State | part、artmesh、glue、draw/render order、opacity、color、flags | 顶点、顺序、可见性、动态 flags |
| C6 Blend Shape | part/warp/rotation/artmesh blend shape 与 constraint | 各层增量和最终 drawable |
| C7 v6 Advanced | offscreen、扩展颜色/alpha blend 等 5.3+ 语义 | offscreen pass 图和完整 snapshot |
| C8 Compatibility | 实际需要的 <code>csm*</code> 调用集 | ABI smoke + 行为回归 |

MVP 的完成面为 C0～C6 中**生产模型实际使用的子集**；没有用到的 feature 也必须被 parser 明确识别并拒绝，不能无声忽略。C7 和 C8 是独立里程碑。性能优化只能在正确性向量稳定后进行，并要求优化前后逐帧差分。

### 6.4 GitHub 参考项目的可用边界

以下结论按 2026-08-21 的仓库状态核验；实现前由 <code>corpus_tool</code> 或维护者重新固定 commit 和 hash。本表是**能力参考**，不是合规门禁。

| 项目 | 已核验能力/限制 | 本项目用途 |
|---|---|---|
| [SakuraMotion/PurismCore](https://github.com/SakuraMotion/PurismCore) | MIT、C99；项目宣称 Core v5/v6 ABI 与 MOC3 5.3 支持。仓库很新；公开可复现测试远少于 README 所述内部测试，公开 CI 无 ESP/RISC-V 真机 | 模块拆分参考、PC 对照进程、性能比较；不把其输出单独当 ground truth |
| [Eatgrapes/Mocari](https://github.com/Eatgrapes/Mocari) | MIT、Rust；0.4.0 含 v1～v6 parser、参数/形变/animation 与 backend-neutral render 辅助，测试面较丰富 | PC oracle、算法边界交叉检查、测试向量生成；依赖 std/文件系统，不直接移植到 ESP 固件 |
| [AyagamiDev/ayagami](https://github.com/AyagamiDev/ayagami) | MIT/Apache-2.0；README 列出至 SDK 5.0 的 loader/driver，API 不稳定且缺 5.3、motion/physics 等 | 参考公开 README 的分层思想与治理规则；其 CONTRIBUTING 禁止 AI 分析/贡献，本项目不向其提交贡献 |
| [vtubing/moc3](https://github.com/vtubing/moc3) | MIT、Rust parser；README 仅列版本 1～5，提交和测试很少 | PC 端 layout 交叉检查；不是 runtime |
| [OpenL2D/moc3ingbird](https://github.com/OpenL2D/moc3ingbird) | 已归档、FDPL-1.0-US；ImHex pattern 到版本 5并明确称未完全验证，仓库含 DoS PoC | 安全审计思路、畸形 offset/count 类别、fuzz corpus 设计；PoC 隔离运行，不进设备 |
| [Ludentes/py-moc3](https://github.com/Ludentes/py-moc3) | MIT、较新、测试少；项目材料表明实现来源含 Java 反编译链路 | 隔离离线交叉检查参考 |
| [QiE2035/moc3-reader-re](https://github.com/QiE2035/moc3-reader-re) | 已归档、明确描述为 reversing；仅 reader | 逆向参考 |

<code>research/reference_manifest.yml</code> 记录：<code>name</code>、<code>url</code>、<code>commit</code>、<code>retrieved_at</code>、<code>license</code>（信息记录）、来源说明。仓库更新不会自动改变已固定用途。

### 6.5 Oracle、差分与安全测试

<code>tools/oracle_runner</code> 使用进程隔离的 JSONL/二进制 sidecar 协议，同一 case 分别运行 self Core 和参考实现（官方 Core 可直接作为 oracle，本地 SDK 已有）。Purism/Mocari/Ayagami 等第三方实现互相不构成正确性证明。可靠结论来自“行为规格 + 自有模型预期 + 至少两个独立实现交叉结果 + 人工审查不一致项”。

每个 case 至少包含：

- 模型/向量 schema version、资源 SHA-256、moc version/feature bitmap、oracle 名称与固定 commit；
- default/min/max/边界外/NaN 拒绝路径、repeat 参数和固定 seed 的参数向量；
- motion/physics 轨迹由 animation 层展开成逐帧 Core 参数，避免把 animation 差异误判为 Core 差异；
- create/update/reset flags/reload 的完整生命周期；
- 静态数组、每层中间结果、最终 vertex/opacity/order/color/flags 和内存计划。

比较规则：

- ID、count、indices、UV bits（若规格要求）、mask、parent、texture index、order 和 flags：精确一致；
- 浮点先比较 finite/符号/边界语义，再比较误差；最终映射到 640×360 时顶点 max ≤0.5 px、P99 ≤0.25 px，opacity/color 绝对误差 ≤1e-4；
- 每个容差都写入 vector schema，禁止为了让测试通过而在实现后放宽；
- 出现 oracle 分歧时 case 进入 quarantine，由规格负责人判定，不用“多数投票”自动生成真值。

测试矩阵：

- 本地 8 个样例、最终生产模型、最小人工模型和每个支持 feature 的单功能模型；
- parser unit/property tests：所有 section 截断点、最大 count、加乘溢出、错位/重叠、坏索引、cycle、NaN/Inf、未知版本/flag；
- host ASan/UBSan/LSan + libFuzzer/AFL++：parser 与 update 各连续至少 24 小时或 1 亿次执行（先达到者），零 crash、越界、泄漏和未分类超时；
- 每模型至少 10,000 次确定性 update，100 次 create/destroy，优化级别与 x86_64/ARM 主机交叉回归；
- ESP 端 heap poisoning、stack canary、watchdog、故障注入和 2 小时 B6；fuzz PoC 只在隔离 host 环境运行。

### 6.6 必须逐级关闭的 Gate

| Gate | 通过条件 | 失败后的动作 |
|---|---|---|
| G-FMT Format/Profile | 生产模型版本/feature/上限冻结；C0～C1 全向量通过；畸形输入均得到稳定错误 | 修 profile、重导出模型或停止 REALTIME |
| G-BHV Behavior | 生产所需 C2～C6 的静态精确项与浮点阈值全部通过；无未解释 oracle 分歧 | 不接 Renderer，不以截图验收代替 |
| G-SEC Security | sanitizer、property/fuzz、100 次生命周期全部通过；解析峰值受预算限制 | 修 parser/runtime 后从 corpus 全量回归 |
| G-TGT Target | ESP32-P4 create/update P95 ≤总帧预算 15%，内存满足第 5 节余量，无未对齐/PSRAM 故障 | 优化或缩 profile；失败则发布 CLIP/STREAM |
| G-RND Rendering | B3/B4/B6 满足第 7.3 节，遮罩/混合/颜色差分通过 | 降分辨率/模型复杂度；失败则不发布 REALTIME |
| G-REL Release | hash 锁定、三配置 CI、2 小时稳定性、异常 fallback 全通过 | REALTIME 保持 Kconfig off |

任一 Gate 失败都不会把研究代码“临时”并入发布。REALTIME 的 build unlock 应由版本化的 <code>generated/core_feature_manifest.h</code> 和 CI 证明共同触发，而不是开发者手改一个宏。

---

## 7. 性能验证，不做无依据承诺

### 7.1 先切换到可复现的 Release 配置

当前工程为 <code>CONFIG_COMPILER_OPTIMIZATION_DEBUG=y</code> / <code>-Og</code>。正式基准应：

- 单独建立并提交 release sdkconfig defaults（已建立 <code>sdkconfig.defaults.release</code>）；
- 使用 performance/release 优化，同时保留必要的栈保护和错误检查；
- 固定 ESP-IDF、工具链、LVGL、self Core/spec/vector schema 和模型包提交；
- 每份报告记录固件 SHA、芯片 revision、CPU/PSRAM 频率和温度。

基准报告必须记录实际 IDF commit 与工具链；不能只引用规范化后的 lock 版本。若迁移稳定版，应单独做显示回归，不与 self Core/Renderer 优化混成一次变更。

### 7.2 基准顺序

| Benchmark | 测什么 | 目的 |
|---|---|---|
| B0 | PSRAM 顺序读写、随机纹理采样、memcpy/fill | 找到带宽上限 |
| B1 | 640×360 → PPA 2×+90° → DSI 双缓冲 | 证明显示链路和 VSYNC |
| B2 | microSD 连续读取 + JPEG→RGB565 + PPA | 证明预渲染路线 |
| B3 | 代表性 textured triangle：nearest/bilinear、alpha、三种 blend | 判断软光栅是否值得继续 |
| B4 | mask atlas 生成与 masked draw | 量化遮罩成本 |
| B5 | self Core inspect/create/update | 量化 blob/IR/arena、初始化峰值与更新时间 |
| B6 | 完整一帧：update + raster + present | 最终端到端结果 |

每个阶段输出 P50/P95/P99，不只输出平均 FPS。B0～B6 固件放入 <code>components/otool_cubism_tool/test/target</code>，直接复用正式 allocator、presenter、metrics 和端口实现，避免“独立 demo 很快、集成后完全不同”。

### 7.3 最低验收线

板端实时路线的初始产品门槛：

- 640×360 档持续 ≥15 FPS；若未达到可试 512×288，但低于 15 FPS 则 No-Go；
- 帧时间 P95 ≤66.7 ms，连续丢帧不超过 2；
- 触摸到参数生效 P95 ≤100 ms；
- 30 分钟压力运行无 watchdog、撕裂、花屏、内存持续下降；
- 2 小时循环后 largest free block 不持续恶化；
- 与 PC 参考截图的像素差异满足预先批准的阈值。

只有真机 B6 数据出来后，才能写“可实现多少 FPS”。

---

## 8. 自研期间的可交付安全网：预渲染交互播放器（路线 D）

主路线已经是 self Core，但它不应让设备几个月都没有可用版本。<code>CLIP_PLAYER</code> 作为研发期演示、发布安全网和 REALTIME 故障 fallback：固件逻辑位于 <code>src/player</code>、<code>src/assets</code> 和 <code>src/presenter</code>，PC 端生成器位于 <code>tools/clip_packer</code>。它不替代 self Core 的里程碑，也不用于伪装连续参数能力。

### 8.1 PC 端素材流水线

用官方 Native SDK 在 PC 上按设备目标尺寸渲染：

1. 画布固定 640×360，背景在 PC 端一并合成。
2. 输出 idle、tap、look-left/right/up/down、blink、talk、happy 等循环/过渡片段。
3. 编码为逐帧 JPEG 或带索引的 MJPEG 素材包。
4. 生成组件自有 manifest：magic、schema version、目标分辨率/像素格式、片段 ID、帧偏移、PTS、循环点、可跳转点、逐帧 CRC 和整包哈希。
5. 素材放 microSD；Flash 只保留短 fallback 动画和 manifest。

JPEG 不带 alpha。最稳的第一版直接合成最终背景；如果必须动态背景，才考虑“彩色 JPEG + 灰度 alpha”双流或其他带 alpha 格式，不能假设 JPEG 可以透明叠加。

设备端只接受明确支持的 manifest major version；未知 major、越界 offset、尺寸不匹配、整数溢出或 CRC 错误都必须在读取数据前拒绝，不能继续“尽量播放”。

### 8.2 板端播放路径

~~~text
otool_cubism_tool/assets：microSD 预读（有界双缓冲）
          │
          ▼
otool_cubism_tool/presenter：硬件 JPEG → 640×360 RGB565
          │
          ▼
PPA 2×缩放 + 90°旋转
          │
          ▼
DSI inactive framebuffer → VSYNC
~~~

Espressif 官方独立测试中，JPEG 硬件解码 1280×720 到 RGB565/RGB888 可达 109 FPS；这说明 codec 余量充足，但 SD、PPA、DSI 并发后的整机结果仍以 B2 真机测试为准。

### 8.3 交互状态机

~~~text
BOOT → IDLE ──tap──> REACT ──end──> IDLE
          │
          ├─drag left/right──> LOOK_L / LOOK_R
          ├─audio level──────> TALK_0..N
          └─timeout──────────> RANDOM_IDLE
~~~

- 在片段的可跳转点切换，避免动作突然截断。
- 输入事件只保留最新状态，队列满时丢旧事件。
- 以 PTS 驱动，不用固定 <code>vTaskDelay</code> 累积漂移。
- SD 读取或 CRC 失败时播放 Flash fallback，不白屏。
- 可用 PPA 固定 alpha 做片段间短交叉淡化，实际支持情况需在 B2 中验证。

### 8.4 MVP 验收

- 640×360 素材稳定 30 FPS 播放；
- 触摸响应 P95 ≤100 ms；
- 连续播放 2 小时无撕裂、卡死和内存下降；
- 拔卡、坏帧、素材版本不匹配均能进入 fallback；
- 记录实际 JPEG 平均/峰值帧大小与 SD 吞吐，再决定素材时长。
- clip-only 配置在没有任何 Core/Framework 文件时可构建、启动和播放。
- 连续切换 LVGL/Cubism 场景及执行组件生命周期 100 次，无任务、buffer 或硬件 client 泄漏。

S0 骨架已经完成；在显示租约可用的前提下，首个组件化预渲染 MVP 仍可按约 **4～7 个工作日**估算。该估算与 self Core 的 3～6 个月研发预算分开统计。

---

## 9. 保留完整实时效果：主机串流（路线 C）

该路线实现为同一组件的 <code>STREAM_CLIENT</code> 模式。若设备可以依赖电脑、小主机或局域网服务器，让官方 SDK 在受支持的平台完成 Core 与 GPU 渲染，组件负责：

- 回传触摸、按键、麦克风幅度等输入；
- 接收带 frame ID / PTS / 长度 / CRC 的 JPEG 帧；
- 队列深度为 1，永远显示最新完整帧；
- 硬件解码、PPA 旋转缩放、VSYNC；
- 断线后切换本地预渲染 idle。

USB/Wi-Fi 具体收发驱动通过 stream port 注入，组件内部只实现统一的 framing、PTS、CRC、背压和重连状态机，避免同时硬依赖两套传输栈。USB 2.0 通常比 Wi-Fi 更容易控制延迟；Wi-Fi 路线还要计算 ESP32-C6 协处理链路和拥塞。先用相同 640×360 JPEG 协议实现，之后再决定是否升到 1280×720。

这条路线保留完整 Live2D 参数、物理、口型和复杂混合效果，也避免在 ESP32 上发布 Core；断线时由 runtime 自动切到组件内置 CLIP fallback。代价是不再是完全独立设备。

---

## 10. 分阶段计划与硬门槛

### 10.1 实施阶段

| 阶段 | 内容 | 交付物 | Gate |
|---|---|---|---|
| S0 组件骨架/基线 | CMake、Kconfig、公共 API、ports、runtime 状态机；固定 IDF/工具链 | **已完成**的 clip-only 骨架 | 构建通过且无 Core 符号；真机 UI 回归仍待补 |
| L0 来源固定 | 固定参考项目 commit/hash（reference_manifest.yml）、冻结生产模型与 profile、建立 spec/vectors 基线 | 来源清单 + spec v1 + corpus manifest + hard limits | G-FMT 的输入冻结 |
| P0 模型/规格 | 冻结字节 5 生产 profile、feature inventory、vector schema、最小 corpus | spec v1 + corpus manifest + hard limits | G-FMT 的输入冻结 |
| C0 安全解析 | bounded reader、validator、不可变 IR、memory plan、host fuzz | self parser v1 + asset_validator | G-FMT + G-SEC |
| C1 Core 基础行为 | static model、parameter、key search、interpolation、确定性 update | C0～C3 vectors 全绿 | G-BHV 对应子集 |
| C2 Core 形变行为 | warp/rotation graph、parts/artmesh/glue/order/flags、生产所需 blend shape | C4～C6 vectors 全绿 | 完整 G-BHV |
| C3 高级兼容（可选） | v6/offscreen/扩展 blend、更多版本、<code>csm*</code> shim | 独立 feature manifest | 各 feature 单独重复 G-FMT/G-BHV/G-SEC |
| A0 Animation | SELF_ANIMATION 的目标 motion/expression/pose/physics，或获准 Framework adapter + 所需 shim | 参数轨迹/事件/生命周期回归 | 展开后的 Core 参数与批准向量一致 |
| D1 显示/降级（可并行） | display lease、B0～B2、CLIP MVP；STREAM 可选 | 可持续演示和 fallback 的设备版本 | 第 8.4 节 |
| R0 软光栅 | renderer、mask、PC 参考图、板端 B3/B4 | 像素差分 + 帧阶段报告 | 第 6.6 节 G-RND |
| I0 REALTIME 集成 | animation/self Core/renderer/presenter/input；包签名与异常恢复 | <code>realtime-self-v5</code> 发布候选 | G-TGT + G-RND + G-REL |

以一名熟悉 C/C++、二进制格式、数值图形和嵌入式性能的资深工程师估算：

- L0 + P0 + C0 约 4～8 人周；
- C1 + C2 + 目标 animation 子集约 6～14 人周；
- R0 + I0 约 4～8 人周，可与后半段 Core 工作部分并行；
- 因此“固定字节 5、固定生产模型、C0～C6 + 单一 animation backend 所需子集”的可发布 REALTIME，按 **12～26 人周（约 3～6 个月）**管理；
- 字节 1～6 全覆盖、C7、完整兼容 ABI、广泛第三方模型和长期兼容承诺，按 **6～12 个月以上**另立项目；
- D1 的 CLIP MVP 仍约 4～7 个工作日，STREAM 在此基础上增加约 3～7 天，二者不计入 self Core “完成度”。

这些是范围预算而非交付承诺；任一 Gate 发现规格缺口、目标模型需要 v6、参考输出不可使用或 Tab5 性能不足，都要重新估算或缩小 profile。

### 10.2 组件化验收线

在功能 Gate 之外，<code>otool_cubism_tool</code> 本身还必须满足：

- <code>idf.py build</code> 至少覆盖 clip-only、clip+stream、realtime-self-v5 三种配置；
- clip-only 的 map/size 结果不包含 Core、Framework、renderer 符号和相关静态数据；
- 除 <code>include/otool_cubism_*.h</code> 外没有私有头文件进入公共 include path；
- 组件目录之外不得 include self Core 私有头、Framework/第三方 Core 头文件或引用其符号；
- 发布固件的 link map 不得出现官方 Core、PurismCore、Mocari 或其他研究 oracle；
- 不存在本机绝对路径、configure 阶段下载和未锁定的分支依赖；
- manifest/moc3/frame parser 与状态机在 host test 中覆盖截断、越界、整数溢出、引用错误、cycle、重复、乱序、CRC/hash/signature 错和版本不兼容；
- display lease、JPEG/PPA 超时、SD 拔出、串流断开和模型加载失败均能回滚到确定状态；
- 100 次完整生命周期后任务数、内部 RAM、PSRAM largest block 和硬件 client 数回到允许误差内；
- <code>generated/core_feature_manifest.h</code> 可追溯到 spec/vector/corpus hash，不能由手改宏解锁；
- <code>reference_manifest.yml</code>、测试报告齐全；
- <code>README.md</code> 明确每个 Kconfig 的 Flash/RAM 影响、实际支持的 moc3 profile 和最小调用示例。

---

## 11. 风险登记

| 风险 | 影响 | 触发信号 | 缓解与退路 |
|---|---|---|---|
| moc3 规格不完整或版本漂移 | 模型加载失败、静默错画 | 新 exporter/字节 6/未知 flag 或 oracle 分歧 | 固定字节 5 生产 profile；未知即拒绝；版本升级重走 G-FMT/G-BHV/G-SEC |
| GitHub 参考项目过新或互相不一致 | 错误规格被当作真值 | Purism/Mocari/Ayagami 输出分歧 | 不单点信任；独立规格、多个 oracle、quarantine 和人工判定 |
| parser 越界/整数溢出 | 崩溃、内存破坏甚至安全漏洞 | malformed corpus 触发 sanitizer/超时 | bounded reader、checked math、hard limits、host fuzz；生产只收签名包 |
| 插值/形变数值漂移 | 动作抖动、错位、遮罩泄漏 | 边界参数或长链误差超过阈值 | 分层中间向量、固定容差、确定性测试；先正确后优化 |
| 目标模型范围扩张 | 工期失控 | 加入更多版本、v6/offscreen/复杂 blend | feature manifest + asset_validator；新能力单独 Gate/排期 |
| 自研 Core 长期维护成本 | 新模型/Exporter 发布后不兼容 | 无规格负责人或回归 corpus 失管 | 明确 owner、版本策略和 corpus retention；只承诺白名单 profile |
| 单组件退化为巨型类 | 修改互相影响、难测试 | facade 包含算法或模块互相反调 | 公共门面只编排；私有模块单向依赖、独立 host test |
| Kconfig 组合失控 | 固件膨胀或混入研究 backend | clip-only 出现 Core；发布 map 出现第三方 oracle | NONE/SELF 收敛、条件源文件、三配置 CI、link map 检查 |
| Renderer 特性扩张 | 工期失控 | 目标模型出现 offscreen/扩展 blend | 模型 profile 门禁；重新导出/简化模型 |
| 纹理内存超限 | 加载失败或碎片 | free/largest block 低于门槛 | 1024²、RGBA4444、按需加载、独占模式 |
| PSRAM 带宽不够 | 帧率低且双核无收益 | B3/B6 tile worker 扩展性差 | 降档、nearest、减少 overdraw；转预渲染 |
| LVGL/Cubism 争用 framebuffer | 撕裂、随机花屏甚至内存破坏 | 仅停止 tick 后直接写 DSI buffer | 先实现可等待 in-flight flush 的显示租约；失败则拒绝 start |
| 当前 IDF 为 beta 且锁文件不一致 | 难复现/显示回归 | 不同机器产物不同 | 固定精确版本；单独验证稳定版迁移 |
| 任意 moc3 输入 | 安全和稳定性风险 | 产品提出用户上传模型 | MVP 禁止；以后需独立威胁模型、沙箱/配额、fuzz 覆盖和产品决策 |
| SD/JPEG 素材故障 | 播放停顿或白屏 | 短读、CRC 错、拔卡 | 有界预读、逐帧 CRC、Flash fallback |

---

## 12. 立即可执行的任务清单

1. 选择唯一生产模型；由 <code>corpus_tool</code> 记录 SHA-256、moc 版本字节、参数/part/deformer/drawable/keyform/mask/offscreen/纹理统计。能重导出时固定为版本字节 5。
2. 在组件中完善 <code>research/reference_manifest.yml</code>、<code>spec/</code>、<code>spec/test_vector_schema.md</code> 和 <code>test/vectors/</code>；固定各参考项目 commit/hash，提交 schema、hard limits 和错误码。
3. 在 host 工具中实现 <code>bounded_reader → validator → immutable IR → memory plan</code>，先让截断/溢出/错引用/cycle/fuzz 测试通过，再共享可移植源码到 ESP 组件。
4. 按 C0～C6 实现 <code>ot_core_*</code>，每完成一层就提交可追溯向量与差分报告；首版不实现 v6/offscreen 和完整 <code>csm*</code> shim。
5. 并行完成 <code>otool_lvgl_idf_port</code> display lease、B0～B2 和 CLIP_PLAYER，使自研研发期间始终有可演示/fallback 版本。
6. G-BHV 与 G-SEC 关闭后，才实现 B3/B4 软光栅；G-TGT/G-RND/G-REL 关闭后才解除 <code>realtime-self-v5</code> 构建门禁。
7. 建立 clip-only、clip+stream、realtime-self-v5 CI matrix，并增加 host sanitizer/fuzz、link map 禁止符号、spec/vector/corpus hash 校验。
8. 只有生产需求明确使用版本字节 6 时，才新建 C7 里程碑并补充 offscreen/扩展 blend 规格、单功能模型，并重新关闭 G-FMT/G-BHV/G-SEC/G-RND。

---

## 13. 参考资料

### 官方资料

- [M5Stack Tab5 规格](https://docs.m5stack.com/en/core/Tab5)
- [Live2D 官方平台支持表](https://docs.live2d.com/en/cubism-sdk-manual/platform/)
- [Live2D Cubism Core API Reference](https://docs.live2d.com/en/cubism-sdk-manual/cubism-core-api-reference/)
- [ESP32-P4 PPA](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html)
- [ESP32-P4 JPEG Codec](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/jpeg.html)
- [ESP32-P4 MIPI-DSI LCD](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/lcd/dsi_lcd.html)
- [ESP-IDF 版本选择说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/versions.html)

### 兼容实现、格式与安全参考

- [SakuraMotion/PurismCore](https://github.com/SakuraMotion/PurismCore)
- [Purism Core v1.0.1 固定提交](https://github.com/SakuraMotion/PurismCore/tree/166785bb1d188bcf11c2dbf1b2476dd31f76a24f)
- [Eatgrapes/Mocari](https://github.com/Eatgrapes/Mocari)
- [Mocari 0.4.0 API 文档](https://docs.rs/mocari/0.4.0/mocari/)
- [AyagamiDev/ayagami](https://github.com/AyagamiDev/ayagami)
- [Ayagami 贡献与治理规则](https://github.com/AyagamiDev/ayagami/blob/main/CONTRIBUTING.md)
- [vtubing/moc3](https://github.com/vtubing/moc3)
- [OpenL2D/moc3ingbird（已归档，含安全 PoC）](https://github.com/OpenL2D/moc3ingbird)
- [moc3ingbird ImHex pattern（标注未完全验证）](https://github.com/OpenL2D/moc3ingbird/blob/master/src/moc3.hexpat)
- [Ludentes/py-moc3（隔离参考）](https://github.com/Ludentes/py-moc3)
- [QiE2035/moc3-reader-re（已归档）](https://github.com/QiE2035/moc3-reader-re)

---

> 项目定位：私有个人项目，不开源、不上市、不发布；允许逆向分析等研究方式。
> 研究来源以 <code>research/reference_manifest.yml</code> 固定的 commit/hash 为准。
> GitHub 仓库状态会变化，使用前重新核验。

---

## 14. 实施记录

> 本节保留每次决策发生时的实际结果。2026-08-21 自研 Core 决策之前的任务编号和 backend 名称属于历史记录，不表示当前目标仍采用 OFFICIAL/PURISM。合规限制移除后，历史记录中"审批/法务"相关描述不再生效。

### 2026-08-21 — S0 组件骨架/基线

**执行范围**：路线调整前计划的组件骨架与构建基线；当前 §10.1 S0。

**已交付**：

| 项 | 内容 | 位置 |
|---|---|---|
| 组件骨架 | CMake/Kconfig/yml/README + 公共头文件 + 状态机实现 | `components/otool_cubism_tool/` |
| 公共 API | 门面类 + 类型（模式/状态/输入/指标）+ 四端口（display/storage/stream/clock） | `include/otool_cubism_{tool,types,port}.h` |
| 状态机 | UNINITIALIZED→READY→LOADED→RUNNING（含 CLIP_FALLBACK/ERROR 预留），完整实现 §4.4 迁移表 | `src/otool_cubism_tool.cpp` |
| runtime | 协调任务（S0：指标采样/心跳，不产帧；通知退出） | `src/runtime/coordinator.cpp` |
| 平台层 | 默认单调时钟端口（esp_timer） | `src/platform/esp_clock.cpp` |
| 输入语义 | DRAG 保留最新状态槽；其他事件走有界队列，满则丢弃并计数 | `src/otool_cubism_tool.cpp` |
| 模式门禁 | STREAM_CLIENT/REALTIME 返回 `ESP_ERR_NOT_SUPPORTED`；REALTIME 开 Kconfig 即构建失败（§4.6 FATAL_ERROR） | `Kconfig` / `CMakeLists.txt` |
| main 集成 | 最小生命周期演示（init→load 失败→start 拒绝→deinit），占位 display 端口 acquire 明确失败，不接管显示 | `main/main.cpp` |
| 版本统一 | `main/idf_component.yml` 固定 `idf: '>=6.1.0-beta1,<6.2.0'`；lock 中 `6.1.0` 系组件管理器对 v6.1-beta1 的规范化表示 | `main/idf_component.yml` |
| release 配置 | `sdkconfig.defaults.release`（PERF 优化 + INFO 日志），用法写入文件头 | `sdkconfig.defaults.release` |

**验证结果**：

- `idf.py build`（IDF v6.1-beta1 / esp32p4）**通过**，`libotool_cubism_tool.a` 生成；
- 产物 `otool_tab5_live2d.bin` 生成；`build/otool_tab5_live2d.map` 中**无任何 Core/Framework 符号**（`csmInitializeModel`/`csmUpdateModel` 零匹配）→ 满足 §10.2 "clip-only 不包含 Core 符号"；
- 烧录至 COM3 成功（`idf.py -p COM3 flash` → Done）；
- ⚠️ **UI 回归未完成真机确认**：烧录后设备 USB 掉线（COM3 枚举 Unknown），无法抓取启动日志确认白屏 + hello onexs. 仍正常。设备重插后可补验（或由用户目视确认）。

**S0 阶段已知简化**（后续阶段修正）：

1. `coordinator_task` 写 `metrics.heap_*` 字段未加锁（32 位写原子，读方可能看到混合值）；S1 引入统一 metrics 锁。
2. `load_package` 仅做"文件存在且非空"校验；manifest 格式（magic/版本/CRC/profile）S2 与 `clip_packer` 同步定义。
3. `stop()` 用轮询 `eTaskGetState()` 等待任务删除（有界超时 2 s）；S1 改为显式任务结束通知。
4. 尚无 host test（§10.2 要求的状态机/解析器测试）；S1 建立 `test/host`。
5. display 端口为占位实现；真实 lease（可等待 in-flight flush 的 acquire/release）待 `otool_lvgl_idf_port` 增加 API 后接入（S1）。

**当时记录的下一步（S1）**：显示租约（改 `otool_lvgl_idf_port` 子模块）→ B0~B2 基准 → 补 COM3 回归验证。

### 2026-08-21 — 主路线调整为 self Core

- **决策**：REALTIME 主路线由官方/Purism 候选切换为组件内独立 <code>SELF_CORE</code>，GitHub 项目降为有来源记录的研究/oracle 角色。
- **范围**：采用固定生产模型、版本字节 5 优先、C0～C6 子集优先；v6/offscreen 和完整 <code>csm*</code> ABI 不进入首版。
- **代码状态**：本次只更新可行性文档，未修改组件代码。现有 Kconfig 已收敛为 NONE/SELF；REALTIME 构建门禁继续生效。
- **下一步**：冻结生产模型和 profile；显示租约/CLIP fallback 可并行推进。

### 2026-08-21 — 自研方向落地：研发治理与规格骨架（原 §12 任务 1~4）

**执行范围**：来源索引、spec 骨架、corpus_tool、Kconfig 收敛。

**已交付**：

| 项 | 内容 | 位置 |
|---|---|---|
| 研究来源索引 | reference_manifest.yml：8 个条目（PurismCore/Mocari/ayagami/vtubing-moc3/moc3ingbird/py-moc3/moc3-reader-re/本地 SDK），固定 commit/hash 记录（工程可复现） | `components/otool_cubism_tool/research/reference_manifest.yml` |
| spec 骨架 | README（规格生命周期 draft/frozen/superseded）+ hard_limits（模型规模/内存/行为上限草案）+ error_codes（ot_core_error_t 分类 + 错误上下文）+ format（moc3_profile_v5 / package_manifest）+ behavior（C0~C6 契约骨架） | `components/otool_cubism_tool/spec/` |
| 测试向量 schema | case/输出/比较规则（精确项 + 浮点容差）/case 类别/存储分发规则 | `spec/test_vector_schema.md` |
| 向量清单 | test/vectors/README + manifest 登记格式（空目录，模型不入库） | `components/otool_cubism_tool/test/vectors/` |
| corpus_tool | PC 端 Python 工具：SHA-256、moc3 magic/版本字节、model3.json+cdi3.json 统计（纹理/参数/部件/动作/表情） | `tools/corpus_tool/corpus_tool.py` |
| Kconfig 收敛 | NONE/OFFICIAL/PURISM → **NONE/SELF**；新增 MOC_PROFILE_V5 / ANIMATION_BACKEND_SELF / APPROVED_FRAMEWORK / CSM_COMPAT / V6_OFFSCREEN；REALTIME 门禁加强（必须 SELF + profile + feature manifest 存在） | `components/otool_cubism_tool/Kconfig` |
| CMake 门禁 | REALTIME 且非 SELF / 无 profile / 无 generated/core_feature_manifest.h / v6 / Framework 无 shim → FATAL_ERROR | `components/otool_cubism_tool/CMakeLists.txt` |
| README 更新 | 组件 README 改为自研方向 + 新 Kconfig 表 + 来源说明 | `components/otool_cubism_tool/README.md` |

**验证结果**：

- corpus_tool 在本地 SDK 8 个样例上跑通；moc3 版本字节与 §3.2 一致（Haru/Natori/Wanko=1，Hiyori/Mark/Rice=3，Mao=5，Ren=6）；统计参数/部件/纹理/动作数正确（例：Mao=132 参数/33 部件/1 纹理，Ren=73/52/1）；
- `idf.py build` clip-only 配置**通过**（REALTIME 关闭，新增 Kconfig 默认值生效）；
- map 文件仍无 Core/Framework 符号；
- REALTIME 门禁按设计 FATAL_ERROR（未实测开启路径，由 CMake 逻辑保证）。

**遗留事项**：

1. 生产模型待用户提供；重导出版本字节 5 后由 corpus_tool 记录统计并冻结 profile。
2. `reference_manifest.yml` 各条目的 commit/hash 待实际固定。
3. C0 安全解析器（现 §12 任务 3）待开始；本轮未写任何格式/行为实现代码。
4. 显示租约/CLIP fallback（现 §12 任务 5）可并行推进，尚未开始。

**下一步**：等待生产模型；并行可做 D1（display lease + B0~B2 + CLIP_PLAYER）。

### 2026-08-21 — 移除合规限制（项目定位：私有，不开源/不上市/不发布）

- **决策**：本项目定位为私有个人项目（不开源、不上市、不发布），允许逆向分析等研究方式；**移除全部法律/合规门禁与审批机制**。
- **删除**：`research/legal_review_sheet.md`（G-LGL 审查单）及其全部引用。
- **简化**：`research/reference_manifest.yml` 由"合规审批清单"改为"研究来源索引"（去掉 allowed_use/approval_ticket 字段，保留 commit/hash 记录）。
- **报告修订**：§3.4 治理边界 → 研究与代码来源策略；§6.6 Gate 表移除 G-LGL；§10.1 L0 由"研发治理"改为"来源固定"；§11/§12 清理法律风险行与审批元数据要求；§13 移除 EULA 链接。
- **保留的工程约束**（与合规无关）：固定 commit/hash、无 configure 阶段联网下载、clip-only 不链接 Core/Framework、REALTIME 构建门禁、组件私有 -fno-exceptions/-fno-rtti。
- **下一步**：冻结生产模型 → C0 安全解析器（现 §12 任务 3）。

---