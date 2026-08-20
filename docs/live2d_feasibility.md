# otool_tab5_live2d — Live2D 上屏与 otool_cubism_tool 组件化方案

> 目标设备：M5Stack Tab5（ESP32-P4、16 MB Flash、32 MB PSRAM、1280×720 MIPI-DSI）
>
> 本地参考：Cubism SDK for Native / Web 5-r.5、当前 ESP-IDF 工程与显示组件
>
> 复核日期：2026-08-21
>
> 状态：**目标架构已收敛为单一 <code>components/otool_cubism_tool</code> 组件；该目录当前为空、尚未实现。预渲染路线可直接实施，纯板端实时路线仍为有条件研发项**

---

## 1. 最终结论

### 1.1 一句话结论

ESP32-P4 **没有官方支持的 RISC-V Cubism Core，也没有 GPU**。因此：

- 要最快、最可靠地做出独立运行的桌宠：采用 **PC 预渲染 + 板端硬件 JPEG 解码 + 交互状态机**。
- 要保留完整实时 Live2D 行为且允许依赖电脑/服务器：采用 **主机实时渲染 + Tab5 串流显示和回传触摸**。
- 要做完全独立、板端实时 Live2D：只能先联系 Live2D 获取官方 RISC-V Core；若无法获得，再对 **Purism Core + CPU 软光栅**做限时验证。只有 Core 等价性、渲染性能、内存稳定性和许可四类 Go/No-Go 门槛全部通过，才进入正式开发。

### 1.2 路线选择

| 路线 | 独立运行 | 连续参数交互 | 可靠性 | 主要风险 | 建议 |
|---|---:|---:|---:|---|---|
| A. 官方 RISC-V Core + 软渲染 | 是 | 是 | 高（若官方提供） | 官方当前未公开提供；软渲染仍需自研 | **先询问 Live2D，最理想** |
| B. Purism Core + 软渲染 | 是 | 是 | 低～中，待实测 | 新项目、无 ESP/RISC-V CI、兼容性与许可需独立确认 | **仅作限时 Spike** |
| C. 主机实时渲染 + JPEG 串流 | 否 | 是 | 中～高 | 依赖主机、网络/USB 延迟 | **需要完整实时效果时推荐** |
| D. 预渲染片段 + 交互状态机 | 是 | 离散状态 | 高 | 不是任意参数连续形变；素材占空间 | **当前最可靠的板端方案** |
| E. 自研 moc3/Core 或在板端跑 Web Core | 是 | 是 | 低 | 格式/算法、性能、维护和许可风险都很高 | **不建议** |

路线 A～D 不是四套分散工程，而是 <code>otool_cubism_tool</code> 的不同构建能力和运行模式。业务层只面对一个公共 API；Core、Framework、软光栅、JPEG 播放、串流、输入映射、显示提交和指标采集都留在组件内部。路线 E 不进入组件。

### 1.3 推荐执行顺序

1. 先建立 <code>components/otool_cubism_tool</code> 骨架、公共 API、Kconfig 和显示端口，再在其中完成 **路线 D**。
2. 同时向 Live2D 书面询问 ESP32-P4/RISC-V Core、嵌入式硬件发布和许可条件。
3. 用最多 2～3 个工作日完成路线 B 的 Core 等价性和板端更新时间 Spike。
4. 再用最多 2～3 个工作日完成代表性软光栅基准。
5. 任一硬门槛失败，组件就保持路线 D，或在允许主机依赖时启用路线 C；**不要直接扩大为“自研 Core”项目**。

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
| IDF 版本 | 构建缓存和编译命令是 **v6.1-beta1**，但 <code>dependencies.lock</code> 写为 <code>6.1.0</code>；需统一并固定版本 |
| Core API | 本地 5-r.5 头文件有 **55 个导出函数**，不是原报告所述“约 10 个” |
| Framework 可编译性 | 排除官方 GPU 后端后，49 个 Framework C++ 源文件已用 ESP RISC-V GCC 15.2、<code>-O2 -fno-exceptions -fno-rtti</code> 静态交叉编译通过 |
| Purism Core 可编译性 | v1.0.1（提交 <code>166785bb…</code>）16 个 C 源文件可用 ESP RISC-V GCC 15.2、<code>-O2 -Werror</code> 编译通过；需显式设置 <code>PSM_HAS_STDINT=0</code> |
| Purism Core 可移植问题 | 不加上述宏时，ESP 工具链因 <code>int32_t</code> 映射为 <code>long</code>、API 返回 <code>int*</code> 而在 <code>param.c</code> 编译失败；必须固定补丁/编译选项并回归 |
| 本地示例资源 | 8 个模型压缩文件总量约 0.7～4.9 MB/模型；moc3 约 0.1～0.9 MB；多数纹理为 2048×2048 |
| Web Core | 本地 Web 5-r.5 提供内联 Emscripten/asm.js 的 <code>live2dcubismcore.js</code>，没有可直接交给嵌入式 WASM Runtime 的独立 <code>.wasm</code> 文件 |
| 目标组件现状 | 当前工作区已有空目录 <code>components/otool_cubism_tool</code>，但没有 CMake、Kconfig、头文件或实现；第 4 节是待实施设计，不代表功能已经存在 |

交叉编译通过只证明“源码能为 RISC-V 生成目标文件”，**不证明模型能正确加载、不证明数值等价、更不证明板端速度足够**。

### 2.2 原报告中必须撤回的判断

| 原判断 | 修正 |
|---|---|
| “Core API 面很小，约 10 个函数” | 本地头文件实际为 55 个导出函数；ABI 兼容、动态标志、遮罩、颜色、离屏绘制都要验证 |
| “moc3 格式与算法有公开文档，复刻本身不侵权” | 官方公开的是 Core API，不等于公开完整 moc3 格式/实现算法；官方专有许可还明确限制逆向工程。不能给出“不侵权”结论 |
| “PurismCore_Old 成熟度中、适配工作量低” | 当前项目已迁至 <code>SakuraMotion/PurismCore</code>；仓库很新且无 ESP32/RISC-V CI，只能作为实验候选 |
| “Framework 直接编入 ESP-IDF 即可” | 基础源文件确实可交叉编译，但仍需统一封装进 <code>otool_cubism_tool</code>，并提供 PSRAM 对齐分配器、日志/文件系统策略和自定义 Renderer 工厂 |
| “RGBA 2048² 纹理 4～8 MB” | 单张 RGBA8888 2048² 是 **16 MiB**；RGBA4444 是 **8 MiB**。Live2D 纹理需要 alpha，不能简单改成无 alpha 的 RGB565 |
| “双核：一核形变、一核光栅可得 1.5～1.8×” | 同一帧光栅依赖 Core 更新后的顶点；直接并行存在依赖和数据竞争。应先更新，再按 tile 并行光栅，收益必须实测 |
| “半分辨率 10～25 FPS、全分辨率 <5 FPS” | 当前没有板端三角形基准，且构建仍为 <code>-Og</code>。这些数字无证据，改为验收门槛而不是预测 |
| “1～2 周完成” | 预渲染 MVP 可按天估算；纯板端实时属于 2～5 周甚至更长的研发项，且可能在 Gate 阶段终止 |

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
- Framework 是源代码可见的 C++ 层，可交叉编译；其许可不是 MIT/Apache，仍需遵守 Live2D Open Software License。
- ESP32-P4 的 PPA 能做矩形缩放、旋转、镜像、填充和图层混合，**不能光栅化带 UV 的三角网格**。
- 所以板端实时路线无论采用哪个 Core，都仍然需要 CPU 软光栅器。

### 3.2 MVP 必须限制模型特性

支持 Cubism 5-r.5 的全部 Renderer 特性会显著扩大工作量，尤其是 5.3 的离屏绘制和扩展颜色/alpha 混合。第一版只接受经过离线校验的模型：

| 约束 | MVP 规则 |
|---|---|
| 画布 | 16:9；首选 640×360 渲染后放大 |
| 纹理 | 最多 2 张；离线缩至不超过 1024² |
| 纹理格式 | 首选预乘 alpha 的 RGBA4444；若边缘质量不足，再评估 RGB565 + A8 |
| 混合 | Normal / Add / Multiply |
| 遮罩 | 支持普通和反相 mask；数量与 atlas 尺寸由基准确定 |
| 5.3 离屏绘制 | MVP 禁止 |
| 扩展颜色/alpha blend mode | MVP 禁止 |
| 模型来源 | 固定、可信、随固件/SD 素材包发布；不接受用户上传任意 moc3 |
| 超限行为 | 离线打包器直接报错，设备端拒绝加载，不能静默降级为错误画面 |

应实现一个 PC 端 <code>asset_validator</code>，输出 Drawable、顶点、三角形、mask、纹理、blend mode、offscreen、moc 版本和预计 RAM；只有通过 profile 的包才允许上板。

### 3.3 Web SDK 不是捷径

本地 Web Core 是面向 JavaScript/WebGL 宿主的 asm.js 产物，且可再分发文件清单只列 JS/类型声明文件。即便引入 QuickJS 等运行时，也会同时带来：

- asm.js 数值计算和 JS↔C/帧缓冲桥接开销；
- 额外堆内存、垃圾回收和启动时间；
- WebGL 仍不存在，Renderer 仍要改写；
- 修改/抽取官方专有产物的许可风险。

因此不把“Web Core + WASM/JS Runtime”列为可交付路线。

---

## 4. 统一 ESP-IDF 组件：components/otool_cubism_tool

### 4.1 组件定位与边界

<code>otool_cubism_tool</code> 是工程中唯一承载 Live2D/Cubism 领域能力的 ESP-IDF 组件。<code>main</code> 只负责创建板级对象、注入端口并选择运行模式，不再直接调用 Cubism Core、Framework、JPEG、PPA 或播放器内部接口。

组件支持三个可独立裁剪、也可同时编入固件的运行能力：

| 运行模式 | 对应路线 | 固件内 Core | 组件内部路径 | 默认 |
|---|---|---:|---|---:|
| <code>CLIP_PLAYER</code> | D | 不需要 | 素材包 → JPEG → presenter | **开启** |
| <code>STREAM_CLIENT</code> | C | 不需要 | 帧协议 → JPEG → presenter | 可选 |
| <code>REALTIME</code> | A/B | 需要一个 backend | Framework/Core → 软光栅 → presenter | Gate 通过后开启 |

统一集成不等于把 PC 工具编译进固件。离线验证、打包和对照工具归属该组件，但放在 <code>tools/</code> 下并从 <code>idf_component_register</code> 的源码列表中排除。

强制边界：

- 公共头文件不得暴露 <code>csmModel*</code>、Framework 类、PPA client、LVGL 对象或第三方容器类型。
- Core backend、Framework、Renderer 和第三方头文件只能由组件私有源码包含。
- 业务代码不得绕过门面调用 Core，也不得直接写 DSI framebuffer。
- <code>CLIP_PLAYER</code> 必须能在完全不链接 Core/Framework 的配置下独立构建。
- 即使启用 <code>REALTIME</code>，<code>CLIP_PLAYER</code> 仍作为加载失败、运行超时或模型不兼容时的安全降级。
- 路线 E 不提供 Kconfig 入口，不允许通过“临时宏”混入发布构建。

### 4.2 建议目录结构

~~~text
components/otool_cubism_tool/
├── CMakeLists.txt
├── Kconfig
├── idf_component.yml
├── README.md
├── LICENSES/                         # 第三方许可文本与书面结论
├── include/
│   ├── otool_cubism_tool.h           # 唯一业务门面
│   ├── otool_cubism_types.h          # 配置、事件、状态、指标
│   └── otool_cubism_port.h           # display/storage/stream 端口
├── src/
│   ├── otool_cubism_tool.cpp         # 生命周期与状态机
│   ├── runtime/                      # coordinator、队列、fallback
│   ├── core/                         # backend-neutral adapter
│   │   ├── core_adapter.cpp
│   │   ├── official_backend.cpp      # 仅获官方 RISC-V Core 后启用
│   │   └── purism_backend.cpp        # 仅 Spike/Gate 通过后启用
│   ├── framework/                    # Framework 包装与 Renderer 工厂
│   ├── assets/                       # manifest、CRC、profile、缓存
│   ├── renderer/                     # tile raster、blend、mask atlas
│   ├── presenter/                    # JPEG、PPA、DSI、VSYNC
│   ├── player/                       # 预渲染片段与交互状态机
│   ├── stream/                       # 帧协议、PTS、CRC、丢旧帧
│   ├── input/                        # touch/audio → 统一事件
│   ├── metrics/                      # P50/P95/P99、堆、丢帧
│   └── platform/                     # ESP-IDF allocator、clock、task
├── vendor/
│   ├── manifest.lock                 # 版本、提交、补丁、SHA-256
│   ├── live2d/                       # 经许可确认的 Framework/Core 内容
│   └── purism_core/                  # 固定版本与本地补丁，可选
├── resources/
│   └── fallback/                     # 可合法分发的最小 Flash 内置片段
├── tools/                            # PC 端，不进入固件
│   ├── core_probe/
│   ├── asset_validator/
│   ├── asset_packer/
│   └── clip_packer/
└── test/
    ├── host/                         # parser、状态机、差分测试
    └── target/                       # B0～B6 与故障注入
~~~

当前 SDK 位于仓库级 <code>third_party/CubismSdk</code>。Spike 阶段可以通过明确的 <code>CUBISM_SDK_ROOT</code> CMake cache 变量引用它，但禁止在组件中写死本机绝对路径。进入 CI/发布前，应通过受控导入脚本把获准文件放入 <code>vendor/live2d</code>，同时生成 <code>manifest.lock</code>；不得在 configure 阶段联网下载、跟随 <code>master</code> 或静默替换版本。

### 4.3 内部模块职责

| 内部模块 | 唯一职责 | 允许依赖 |
|---|---|---|
| facade/runtime | 生命周期、状态迁移、错误恢复、模式切换 | 其余内部接口 |
| core | 抹平官方/Purism ABI；输出组件自有 draw snapshot | vendor Core |
| framework | motion、expression、physics、model 生命周期 | core、assets |
| assets | 可信包、manifest/profile/CRC、按需读取 | storage port |
| renderer | 纹理三角形、blend、mask、tile worker | draw snapshot、assets |
| presenter | JPEG decode、PPA scale/rotate、VSYNC present | display port、IDF driver |
| player | 预渲染片段调度、循环点、交互跳转 | assets、presenter |
| stream | 帧拆包、PTS、CRC、队列深度 1 | stream port、presenter |
| input | 触摸/音量/命令归一化，不直接改 Core | runtime queue |
| metrics | 分阶段耗时、内存、丢帧、错误计数 | 只读观测点 |

内部依赖必须保持单向；例如 presenter 不得反调 player，renderer 不得读取 LVGL 全局状态，Core backend 不得自行打开文件。

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

建议至少提供：

| 配置 | 规则 |
|---|---|
| <code>CONFIG_OTOOL_CUBISM_ENABLE_CLIP_PLAYER</code> | 默认开启 |
| <code>CONFIG_OTOOL_CUBISM_ENABLE_STREAM_CLIENT</code> | 默认关闭 |
| <code>CONFIG_OTOOL_CUBISM_ENABLE_REALTIME</code> | 默认关闭，Gate 通过后才允许发布配置开启 |
| <code>CONFIG_OTOOL_CUBISM_CORE_BACKEND_*</code> | NONE/OFFICIAL/PURISM 三选一；REALTIME 禁止 NONE |
| <code>CONFIG_OTOOL_CUBISM_RENDER_SIZE_*</code> | 640×360 / 512×288 / 320×180 三选一 |
| <code>CONFIG_OTOOL_CUBISM_RASTER_WORKERS</code> | 1 或 2，由 B3/B6 决定 |
| <code>CONFIG_OTOOL_CUBISM_METRICS</code> | 开发默认开启，发布保留低成本计数 |

<code>CMakeLists.txt</code> 按 Kconfig 追加源文件，而不是把所有 backend 编译后再在运行时选择。配置阶段必须执行以下检查：

- 启用 REALTIME 却没有唯一 Core backend：直接 <code>FATAL_ERROR</code>。
- 选择 OFFICIAL 但缺少获准的 RISC-V 库、头文件或哈希不符：直接失败。
- 选择 PURISM 但版本/补丁/宏不匹配：直接失败。
- clip-only 构建不得出现 Core/Framework 未解析符号。
- host tools、测试模型和官方示例素材不得进入固件镜像。
- 仅 <code>resources/fallback</code> 中经许可确认且通过哈希校验的最小片段可用 <code>EMBED_FILES</code> 进入 Flash。
- 组件私有 C++ 统一使用 <code>-fno-exceptions -fno-rtti</code>；不要修改整个工程的编译语义。

CI 至少构建 <code>clip-only</code>、<code>clip+stream</code> 和一个获准的 <code>realtime</code> 配置，并保存 <code>idf.py size-components</code> 结果。

### 4.7 三条运行路径汇合到同一 presenter

~~~text
CLIP_PLAYER：   manifest/SD → clip scheduler → JPEG ─────────┐
STREAM_CLIENT： transport → frame parser   → JPEG ──────────┼→ presenter
REALTIME：      model → Framework/Core → soft renderer ─────┘
                                                        │
                                                        ▼
                                      PPA scale/rotate → DSI → VSYNC
~~~

实时路径内部为：

~~~text
参数/动作/物理
      │
      ▼
Core 单线程更新
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
| Motion/Physics/Core update | 单线程 | 保证模型状态一致 |
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
| moc3 可写对齐副本 | 0.1～0.9 MiB（示例范围） | Framework 通常会复制到对齐、可写内存，不能把压缩文件大小当零开销 |
| Core model 工作区 | **待实测** | 由 <code>csmGetSizeofModel</code> 记录 |
| Framework/Motion/Physics | **待实测** | JSON、容器和动作切换可能造成峰值及碎片 |
| FreeRTOS 栈、队列、指标 | 待实测 | 栈优先放内部 RAM |
| 安全余量 | 至少 6 MiB 或总 PSRAM 的 20%，取较大者 | 用于碎片、峰值和恢复 |

<code>CLIP_PLAYER</code> 和 <code>STREAM_CLIENT</code> 不应分配 moc/model、纹理和 mask atlas，只保留有界 JPEG 输入缓冲、RGB565 场景缓冲及 presenter 资源。CMake 未启用 REALTIME 时，不得为了“将来可能使用”而静态预留实时路径内存。

禁止把 2048² PNG 的文件大小当作运行时纹理大小：

- 2048² RGBA8888：16 MiB/张；
- 2048² RGBA4444：8 MiB/张；
- 两张 RGBA8888 已经达到整机 32 MiB PSRAM，尚未计显示和模型。

### 5.3 分配策略

- 在组件 <code>platform/</code> 中实现自定义 <code>ICubismAllocator</code>：大于阈值的模型/纹理/帧缓冲放 PSRAM，小对象和任务栈留内部 RAM。
- 所有 PPA、JPEG、Core 要求的 buffer 使用规定的地址和大小对齐；不要假定普通 <code>malloc</code> 足够。
- 启动、模型加载、动作切换和运行 30 分钟后分别记录：
  - 内部 RAM / PSRAM free、minimum free、largest free block；
  - Core moc/model 实际申请；
  - 每类纹理/mask/framebuffer 实际字节数。
- 运行时不解析/保留原始 PNG；PC 端转换为设备格式，可选 LZ4 压缩，启动时一次解压到 PSRAM。
- motion/physics 先限制数量并按需加载；若 JSON 峰值明显，再引入离线二进制格式。
- 连续执行至少 100 次 <code>init → load → start → stop → deinit</code>，确认内部 RAM、PSRAM、largest free block、PPA/JPEG client 和任务数量回到基线。

---

## 6. Core 候选的可靠验证

### 6.1 候选顺序

1. **向 Live2D 申请/询问官方 RISC-V 构建**。官方 Core API 本身刻意避免内部内存分配，理论上适合嵌入式，但是否提供只能以官方书面回复为准。
2. 官方无法提供时，Spike 使用 **SakuraMotion/PurismCore v1.0.1**，固定提交 <code>166785bb1d188bcf11c2dbf1b2476dd31f76a24f</code>。
3. <code>moc3-reader-re</code> 已归档且主要是文件结构读取器，不是可替换 Core，不作为后备运行时。
4. 不把“自行补齐解析/形变”作为失败后的默认备选；那是一个独立的长期项目。

### 6.2 PC 双实现对照工具

在 <code>components/otool_cubism_tool/tools/core_probe</code> 编写同一个对照协议，分别链接官方 Core 和候选 Core，给定相同 moc3 与参数序列，导出：

- Core/MOC 版本、canvas、参数/part/drawable 数量与 ID；
- 参数范围、默认值、repeat/key；
- indices、UV、texture index、mask、parent、constant flags；
- 每步 update 后的 vertex、opacity、draw/render order、dynamic flags；
- multiply/screen color、blend mode、offscreen 信息；
- moc 大小、model 工作区大小和单次 update 时间。

测试集合不能只有 Haru：

- 本地 8 个官方示例模型；
- 最终准备发布的目标模型；
- default / min / max / 边界外参数；
- 固定随机种子的参数向量；
- 完整 motion + physics 轨迹；
- 重复 update、reset dynamic flags 和动作切换。

### 6.3 Core Gate

| 检查 | 通过条件 |
|---|---|
| ABI | 所需符号、枚举、结构大小、调用约定一致 |
| 静态数据 | count、ID、indices、UV、mask、texture index 完全一致 |
| 动态整数/标志 | order、flags、可见性完全一致 |
| 浮点 | 映射到 640×360 后，顶点最大误差 ≤0.5 px、P99 ≤0.25 px；opacity/color 误差 ≤1e-4 |
| 稳定性 | 每模型至少 10,000 次 update，无崩溃、越界、堆持续下降 |
| 板端速度 | Core update P95 不超过总帧预算的 15% |
| 板端内存 | 模型加载后满足第 5 节安全余量和 largest-block 要求 |
| 许可 | 发布方式、Framework、Core 替代实现和模型素材均获负责人/法律确认 |

任何一项失败都停止路线 B，并保持 <code>CONFIG_OTOOL_CUBISM_ENABLE_REALTIME=n</code>；不影响组件的 CLIP/STREAM 能力，也不以“肉眼看起来差不多”放行。

---

## 7. 性能验证，不做无依据承诺

### 7.1 先切换到可复现的 Release 配置

当前工程为 <code>CONFIG_COMPILER_OPTIMIZATION_DEBUG=y</code> / <code>-Og</code>。正式基准应：

- 单独建立并提交 release sdkconfig defaults；
- 使用 performance/release 优化，同时保留必要的栈保护和错误检查；
- 固定 ESP-IDF、工具链、LVGL、Core、模型包提交；
- 每份报告记录固件 SHA、芯片 revision、CPU/PSRAM 频率和温度。

先解决当前 “IDF 路径为 v6.1-beta1、lock 为 6.1.0” 的不一致。若迁移稳定版，应单独做显示回归，不与 Live2D 优化混成一次变更。

### 7.2 基准顺序

| Benchmark | 测什么 | 目的 |
|---|---|---|
| B0 | PSRAM 顺序读写、随机纹理采样、memcpy/fill | 找到带宽上限 |
| B1 | 640×360 → PPA 2×+90° → DSI 双缓冲 | 证明显示链路和 VSYNC |
| B2 | microSD 连续读取 + JPEG→RGB565 + PPA | 证明预渲染路线 |
| B3 | 代表性 textured triangle：nearest/bilinear、alpha、三种 blend | 判断软光栅是否值得继续 |
| B4 | mask atlas 生成与 masked draw | 量化遮罩成本 |
| B5 | Core load/update | 量化模型工作区与更新时间 |
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

## 8. 最可靠的 MVP：预渲染交互播放器（路线 D）

该路线实现为 <code>otool_cubism_tool</code> 的 <code>CLIP_PLAYER</code> 模式：固件逻辑位于 <code>src/player</code>、<code>src/assets</code> 和 <code>src/presenter</code>，PC 端生成器位于 <code>tools/clip_packer</code>。

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

包含空组件骨架、公共 API、显示租约和测试后，首个组件化预渲染 MVP 预计 **4～7 个工作日**。这仍比“先做完整 Core + Renderer”更适合作为第一交付物。

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
| S0 组件骨架/基线 | 建立 CMake、Kconfig、公共 API、ports、runtime 空状态机；固定 IDF/工具链 | clip-only 空实现可复现构建 | 无 Core 链接；现有 UI 回归通过 |
| S1 显示租约/基准 | 完成 LVGL display lease；在组件 target tests 跑 B0～B2 | CSV/日志 + 正式 presenter | 100 次租约循环；预渲染 30 FPS 通过 |
| S2 CLIP MVP | assets/player/presenter、clip_packer、交互和 Flash fallback | 单组件可演示桌宠 | 第 8.4 节全部通过 |
| S2b STREAM 可选 | stream port、framing、PTS/CRC、断线 fallback | 主机实时串流 | 延迟、断线恢复和长稳通过 |
| S3 Core Spike | 官方询问；组件 core backend + core_probe；板端 load/update | Core 对比报告 | 第 6.3 节全部通过 |
| S4 Renderer Spike | 组件 renderer + PC 参考图 + 板端 B3/B4 | 差分图和帧阶段报告 | 目标档 ≥15 FPS 的预算可成立 |
| S5 REALTIME 集成 | Framework、Core、renderer、presenter、输入 | 同一公共 API 下的实时模式 | B6 和 30 分钟稳定性通过 |
| S6 产品化 | 模式切换、异常恢复、包工具、长稳、许可与 CI matrix | 组件发布候选 | 2 小时、许可、三配置回归全通过 |

粗略工作量：

- 组件骨架 + 预渲染 MVP：4～7 天；
- 主机串流：在 S2 基础上增加 3～7 天；
- 纯板端实时：在 S3/S4 均通过后，仍应按 **2～5 周研发**评估，而不是承诺 1～2 周。

### 10.2 组件化验收线

在功能 Gate 之外，<code>otool_cubism_tool</code> 本身还必须满足：

- <code>idf.py build</code> 至少覆盖 clip-only、clip+stream、获准的 realtime 三种配置；
- clip-only 的 map/size 结果不包含 Core、Framework、renderer 符号和相关静态数据；
- 除 <code>include/otool_cubism_*.h</code> 外没有私有头文件进入公共 include path；
- 组件目录之外不得直接 include Core/Framework/Purism 头文件或引用其符号；
- 不存在本机绝对路径、configure 阶段下载和未锁定的分支依赖；
- manifest parser、状态机、frame parser 在 host test 中覆盖截断、越界、重复、乱序、CRC 错和版本不兼容；
- display lease、JPEG/PPA 超时、SD 拔出、串流断开和模型加载失败均能回滚到确定状态；
- 100 次完整生命周期后任务数、内部 RAM、PSRAM largest block 和硬件 client 数回到允许误差内；
- <code>README.md</code> 明确每个 Kconfig 的 Flash/RAM/许可影响，并给出最小调用示例。

---

## 11. 风险登记

| 风险 | 影响 | 触发信号 | 缓解与退路 |
|---|---|---|---|
| 无官方 RISC-V Core | 阻断官方板端实时路线 | Live2D 明确无法提供 | Purism 限时 Spike；失败转预渲染/串流 |
| Purism 项目成熟度不足 | 错画、崩溃、升级困难 | PC 数值对照或长稳失败 | 固定版本、全模型回归；失败即停 |
| 许可/模型授权不明确 | 无法发布 | 无书面结论或业务规模触发授权 | 发布前联系 Live2D 及法律负责人；不做“不侵权”假设 |
| 单组件退化为巨型类 | 修改互相影响、难测试 | facade 包含算法或模块互相反调 | 公共门面只编排；私有模块单向依赖、独立 host test |
| Kconfig 组合失控 | 固件膨胀或链接错误 backend | clip-only 仍出现 Core 符号 | 条件源文件、三配置 CI、size-components 对比 |
| Renderer 特性扩张 | 工期失控 | 目标模型出现 offscreen/扩展 blend | 模型 profile 门禁；重新导出/简化模型 |
| 纹理内存超限 | 加载失败或碎片 | free/largest block 低于门槛 | 1024²、RGBA4444、按需加载、独占模式 |
| PSRAM 带宽不够 | 帧率低且双核无收益 | B3/B6 tile worker 扩展性差 | 降档、nearest、减少 overdraw；转预渲染 |
| LVGL/Cubism 争用 framebuffer | 撕裂、随机花屏甚至内存破坏 | 仅停止 tick 后直接写 DSI buffer | 先实现可等待 in-flight flush 的显示租约；失败则拒绝 start |
| 当前 IDF 为 beta 且锁文件不一致 | 难复现/显示回归 | 不同机器产物不同 | 固定精确版本；单独验证稳定版迁移 |
| 任意 moc3 输入 | 安全和稳定性风险 | 支持用户上传模型 | MVP 禁止；只加载签名/CRC 的可信素材包 |
| SD/JPEG 素材故障 | 播放停顿或白屏 | 短读、CRC 错、拔卡 | 有界预读、逐帧 CRC、Flash fallback |

---

## 12. 立即可执行的任务清单

1. 记录并统一实际 ESP-IDF 版本；建立 release 性能配置。
2. 在现有空目录中建立 <code>otool_cubism_tool</code> 的 CMake、Kconfig、公共头文件、ports、runtime 状态机和 clip-only 构建。
3. 为 <code>otool_lvgl_idf_port</code> 设计并验证 display lease；组件在租约不可用时必须拒绝接管显示。
4. 把 B0～B2 放入组件 <code>test/target</code>，先测显示、PPA、JPEG、SD、生命周期和内存。
5. 在组件 <code>tools</code> 下建立 <code>clip_packer</code>，用官方 PC SDK 生成 640×360 的 idle/tap/look 包，完成 CLIP_PLAYER。
6. 向 Live2D 提交 RISC-V Core 与发布许可咨询。
7. 在组件 <code>tools</code> 下建立 <code>core_probe</code> 和 <code>asset_validator</code>，先做 PC 对照并冻结目标模型。
8. 只有 Core Gate 通过后才启用 REALTIME Kconfig 并实现 B3/B4 软光栅 Spike。
9. 建立 clip-only、clip+stream、realtime CI matrix；任何实时路径故障都回到组件内置 CLIP fallback。

---

## 13. 参考资料

### 官方资料

- [M5Stack Tab5 规格](https://docs.m5stack.com/en/core/Tab5)
- [Live2D 官方平台支持表](https://docs.live2d.com/en/cubism-sdk-manual/platform/)
- [Live2D Cubism Core API Reference](https://docs.live2d.com/en/cubism-sdk-manual/cubism-core-api-reference/)
- [Live2D Proprietary Software License Agreement](https://www.live2d.com/eula/live2d-proprietary-software-license-agreement_en.html)
- [Live2D Open Software License Agreement](https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html)
- [Live2D Free Material License Agreement](https://www.live2d.com/eula/live2d-free-material-license-agreement_en.html)
- [ESP32-P4 PPA](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/ppa.html)
- [ESP32-P4 JPEG Codec](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/jpeg.html)
- [ESP32-P4 MIPI-DSI LCD](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/api-reference/peripherals/lcd/dsi_lcd.html)
- [ESP-IDF 版本选择说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32p4/versions.html)

### 候选与历史参考

- [SakuraMotion/PurismCore](https://github.com/SakuraMotion/PurismCore)
- [Purism Core v1.0.1 固定提交](https://github.com/SakuraMotion/PurismCore/tree/166785bb1d188bcf11c2dbf1b2476dd31f76a24f)
- [已归档的 moc3-reader-re](https://github.com/QiE2035/moc3-reader-re)

---

> 许可部分只是工程风险提示，不构成法律意见。尤其是第三方 Core 重实现、Framework 分发、官方示例模型和最终商业发布，必须依据届时有效的协议及书面咨询结果处理。

---

## 14. 实施记录

> 本表记录对 §12 任务清单的实际执行情况。每一条记录包含：日期、范围、结果、验证方式、遗留事项。

### 2026-08-21 — S0 组件骨架/基线

**执行范围**：§12 任务 1（部分）、任务 2；§10.1 S0。

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

**下一步（S1）**：显示租约（改 `otool_lvgl_idf_port` 子模块）→ B0~B2 基准 → 补 COM3 回归验证。

---

*本报告基于 2026-08 检索信息与本地 SDK 5-r.5 实物分析；实施前建议复核各开源仓库的最新状态。*
