# otool_cubism_tool 画面保真度与看门狗恢复计划

> 对象：`components/otool_cubism_tool`
>
> 目标模型：Mao（moc3 版本字节 5）
>
> 目标设备：M5Stack Tab5 / ESP32-P4 / 1280×720
>
> 分析日期：2026-08-21
>
> 文档性质：根因分析、实施顺序和验收 Gate；不是“当前已经达到原版效果”的声明。

## 0. 执行结论

当前画面与原版差距巨大，首要原因不是分辨率或 RGB565，而是两项会直接破坏模型结构的运行时语义错误：

1. **父 deformer 的 opacity 没有传递给 art mesh。** 官方 Core 默认帧只有 150/262 个 drawable 可见，self Core 却绘制了 217/262 个；多出的 **67 个 drawable** 全部位于 opacity 为 0 的父 deformer 链下。它们本应完全隐藏，却以备用姿态、特效片段和不完整纹理块的形式覆盖在人物上。
2. **Renderer 使用 draw order 稳定排序代替官方 render order。** 默认帧中 169/262 个 drawable 的最终 rank 不一致，其中 **89 个是可见 drawable**，最大 rank 偏差 85 层。即使顶点和纹理都正确，也会出现头发、脸、衣服、手臂前后关系错误。

这两项应作为 P0 在任何纹理升级或性能优化之前修复。完成后，默认帧的大部分“碎片化、黑洞、错误覆盖”应消失。

第二层差距来自：blend shape/glue/深层 deformer 语义不完整、mask 与三角形边规则不完全等价、nearest 纹理采样、RGBA4444 量化，以及 640×360 帧被无抗锯齿放大到 1280×720。

看门狗问题与错误画面不是两个独立故障。67 个本应隐藏的 drawable 被光栅化，会额外执行三角形遍历、纹理采样和 mask；当前 37 个 masked target 还会逐个清空完整 640×360 A8 mask。CPU0 长时间停在 `raster_tri()`，IDLE0 无法运行，最终触发 5 秒 TWDT。

## 1. 分析基线与方法

### 1.1 对照对象

本次使用同一份 Mao.moc3、同一组默认参数，对比：

- 官方 Windows x64 Cubism Core 5-r.5 的单帧输出；
- 仓库 self Core 的 `core_update_frame()` 输出；
- 当前软光栅与 1024² RGBA4444 纹理的 host 预览；
- 用户提供的原版效果截图，仅作为视觉目标，不作为像素级 oracle。

官方 Core 研究产物和模型保留在 `%TEMP%/otool_cubism_research/`，不纳入组件。仓库内 `c0_probe --dump-runtime` 输出 TSV，便于继续做逐 drawable 回归。

### 1.2 Mao 模型实际复杂度

| 项目 | 数量 | 对当前实现的含义 |
|---|---:|---|
| Parameters | 132 | 不能只用 4 个 demo 参数证明运行时正确 |
| Parts | 32 | pose/part opacity 需要进入完整状态链 |
| Deformers | 178 | 最大父链长度 15，近似的一跳组合不够稳健 |
| Warp / Rotation | 118 / 60 | 深层 warp/rotation 混合是主要数值风险 |
| Art meshes | 262 | 最终需要逐项 opacity、顶点、颜色、render rank 对照 |
| Vertices / triangle indices | 6020 / 23829 | 默认约 7943 个三角形进入候选集合 |
| Masks | 65 个引用 | 37 个 masked target，实际只有 16 组唯一 mask 集合 |
| Draw groups / items | 2 / 263 | 官方最终 render order 不能由 raw draw order 代替 |
| Glues / glue info | 7 / 322 | 当前 update 未实现 |
| Blend shape axes / bindings | 33 / 124 | 当前 update 明确忽略 blend shape |
| Blend shape targets | warp 3、art mesh 31、part 1、rotation 3 | 影响静态细节和参数运动 |
| Multiply / screen color keyforms | 2232 / 2232 | 默认可见集合为 identity，但动作/特效状态会用到 |

### 1.3 默认帧逐 drawable 对照结果

| 指标 | 结果 | 判断 |
|---|---:|---|
| Drawable 总数 | 262 | 两端结构数量一致 |
| 顶点总数 | 6020 | 两端结构数量一致 |
| 官方可见 drawable | 150 | 基准 |
| self Core 可见 drawable | 217 | **错误多绘制 67 个** |
| Opacity 不一致 | 67/262 | 67 个均为 official=0、self>0 |
| Opacity 错误与父链关系 | 67/67 的祖先链含 opacity=0 deformer | 已定位为未传播父 deformer opacity |
| Draw order | 262/262 数值一致 | raw draw order 计算本身不是当前主错 |
| 最终 render rank | 169/262 不一致 | **排序算法/Draw Group 语义缺失** |
| 可见 drawable 的 rank 错误 | 89/150 | 足以破坏前后遮挡关系 |
| 最大 rank 偏差 | 85 | 不是相邻层的小误差 |
| 顶点最大误差 ≤1e-6 | 237/262 drawable | 基础参数插值和大部分形变已接近正确 |
| 顶点最大误差 ≤1e-5 | 241/262 drawable | 21 个仍需处理 |
| 官方可见顶点 RMS | 0.000529 model unit，约 0.132 个 640×360 像素 | 大部分可见几何已接近官方 |
| 官方可见顶点最大误差 | D67，约 4.11 个 640×360 像素 | 深层 deformer 组合仍有肉眼可见偏差 |

可见集合中超过 `1e-5` model unit 的 drawable 只有 6 个：D67、D75～D79。其中 D75～D79 都是 blend shape art mesh target；D67 与它们共享较深的 warp/rotation 父链。另有 15 个 drawable 约偏移 1.358 model unit，但官方默认 opacity 为 0；它们当前仍可能因 opacity 传播缺失而进入错误画面。

### 1.4 从数据到截图症状的因果链

~~~text
moc3 默认参数
   │
   ├─ self Core 未传播父 deformer opacity
   │      └─ 67 个备用/特效 drawable 被错误显示
   │
   ├─ 只按 draw order 稳定排序
   │      └─ 89 个可见 drawable 前后层级错误
   │
   ├─ blend shape / glue / 深层组合不完整
   │      └─ 局部轮廓和动作状态偏移
   │
   └─ nearest + RGBA4444 + 640×360→1280×720
          └─ 锯齿、色阶、细节和边缘质量下降
~~~

因此当前截图中的大面积碎片和缺口主要是**状态/排序正确性**问题；锯齿和粗糙感才是**采样/像素格式**问题。两类问题必须分阶段解决。

## 2. 根因分解

### 2.1 P0：父 deformer opacity 未进入 mesh 最终 opacity

`moc3_update.cpp` 已计算 `warp_opacity`、`rot_opacity` 和局部的 `def_opacity_accum`，但 art mesh 阶段只写入自身 keyform opacity：

~~~text
mesh_opacity = interpolate(art_mesh_key_opacity)
~~~

缺失的语义是：

~~~text
mesh_opacity = art_mesh_local_opacity
             × parent_deformer_accumulated_opacity
             × part/pose opacity（进入 Framework 阶段后）
~~~

证据不是相关性推测：67 个 opacity mismatch 的 drawable，其父 deformer 祖先链 **67/67 均存在输出 opacity=0 的节点**；其中 59 个直接父节点就是 0，另 8 个由更高祖先归零。

这个错误同时解释两件事：

- 为什么 atlas 中大量不该出现的局部碎片被画到人物上；
- 为什么软光栅工作量异常增大并加剧 WDT。

### 2.2 P0：draw order 与 render order 被混为一谈

当前 `model_render.cpp` 对 `mesh_draw_order` 做稳定插入排序。官方 Core 则通过 Draw Group/Draw Object 关系产生完整的 `render order` 排列，Framework 直接按该 rank 绘制。

Mao 默认帧的 raw draw order 数值已全部匹配官方，但由它稳定排序得到的 rank 仍有 169/262 项不匹配。这说明相同 draw order 内的次序、group 边界和 group 内对象关系不能用文件顺序补齐。

修复目标不是“换一种 C++ sort”，而是实现 moc3 中以下字段的运行语义：

- `SLOT_DG_OBJ_BEGIN / COUNT / TOTAL`；
- `SLOT_DG_MAX_ORDER / MIN_ORDER`；
- `SLOT_DGO_TYPE / INDEX / SELF_GROUP`；
- 本帧 draw order 变化后重建或增量更新 render rank。

### 2.3 P1：深层 deformer、blend shape 与 glue 尚未闭环

当前更新器将深层父变换组合在一个大函数内，并对 rotation-under-parent 使用方向探针近似父角度。Mao 的 deformer 链最大长度为 15；D67 在默认可见帧仍有约 4.11 像素最大偏差，说明近似组合需要替换为可验证的变换表示。

同时，当前头文件明确声明“无 blend shape”，而 Mao 包含：

- 33 blend shape axes；
- 124 blend shape bindings；
- 31 art mesh、3 warp、3 rotation、1 part target；
- 7 constraints、14 constraint values。

D75～D79 的默认可见误差与 blend shape target 直接重合。7 组 glue/322 条 glue info 也尚未进入顶点更新。默认帧可能只暴露少量误差，但动作、表情和物理驱动后误差会显著扩大。

Rotation 的 reflect X/Y keyform 当前也未进入最终变换，必须纳入参数扫描 oracle。

### 2.4 P1：mask 与三角形覆盖规则仍是近似实现

当前已经支持普通/反相 mask，但仍有以下等价性风险：

- 每个 masked target 都重新清空整张 640×360 A8 mask；37 次清空至少产生 `37 × 230400 = 8,524,800` 次字节写入/帧；
- 37 个 target 实际只有 16 组唯一 mask 集合，存在明显重复计算；
- 当前 mask 绘制乘入 mask drawable 的 runtime opacity，而官方 SetupMask shader 直接使用纹理 alpha；需用 oracle 固定最终规则；
- 三角形内部判断对三条边都使用 `>=0`，没有 GPU 风格 top-left fill rule，共享边可能被两个三角形重复混合；
- A8 mask 虽比原始 A4 alpha 精细，但源纹理 alpha、nearest 采样和边覆盖规则仍会造成轮廓破损；
- 当前每个目标使用全屏 mask，数学上可行，但与官方 clipping context 的局部矩形/atlas 策略性能差距大。

### 2.5 P1：纹理与显示链主动牺牲了质量

当前资源链为：

~~~text
2048×2048 RGBA8888 PNG
  → LANCZOS 缩至 1024×1024
  → 预乘 RGBA4444（2 MiB）
  → 软光栅 nearest
  → 640×360 RGB565
  → LVGL 无抗锯齿拉伸到 1280×720
~~~

这里有四次质量损失：

1. atlas 线性尺寸减半；
2. RGB 和 alpha 都从 8 bit 降到 4 bit；
3. minification/magnification 都使用 nearest；
4. 最终每个逻辑像素被 2× 放大，且 `lv_image_set_antialias(false)`。

这些因素会产生色阶、边缘锯齿和小五官细节丢失，但不会单独造成当前那种大范围错误纹理块。应在 P0 结构正确后，用同参数、同分辨率的官方 host 帧重新评估。

### 2.6 P1：当前 demo 与“原版运行状态”并非同一条动画链

Mao 资源还包含：

- `Mao.pose3.json`；
- `Mao.physics3.json`；
- 8 个 expression；
- 8 个 motion；
- EyeBlink、LipSync group。

当前 demo 只直接修改 `AngleX`、`AngleY`、左右眼开合 4 个参数，没有执行 pose、motion、expression、physics 或 lip sync。即使 Core 和 Renderer 全部正确，也只能和“相同 4 参数输入下的官方静态帧”比较，不能直接声称等价于原版播放器截图。

### 2.7 P0：WDT 是不可抢占热循环和重复工作的共同结果

原始回溯明确停在 `raster_tri()`，CPU0 正在运行 `cubism_demo`，IDLE0 超过 5 秒没有获得执行机会。当前已经加入协作回调、任务降优先级和帧后延时，这属于必要保护，但仍需要用性能结构消除根因：

- 先跳过 67 个错误可见 drawable；
- 避免 37 次全屏 mask 清空与重复 mask raster；
- 将每个不可中断 CPU slice 限制在明确的时间预算内；
- 用固定点/增量式内插替换每像素 float 热路径；
- 以 deadline 驱动帧率，超预算时降帧而不是霸占 CPU；
- 保持 TWDT 开启，不能以延长/关闭 watchdog 作为修复。

## 3. 优先级矩阵

| 优先级 | 工作项 | 对画面影响 | 对 WDT 影响 | 先决条件 |
|---|---|---:|---:|---|
| P0.1 | 父 deformer opacity 传播 | 极高 | 高 | 无 |
| P0.2 | 实现官方等价 render order | 极高 | 中 | P0.1 后重新出图 |
| P0.3 | 建立官方 Core/Renderer 固定输入 oracle | 极高 | 低 | 无 |
| P0.4 | 时间预算式 cooperative yield + 指标 | 低 | 极高 | 无 |
| P1.1 | 深层 deformer 精确组合 | 中～高 | 低 | P0 Gate |
| P1.2 | blend shape / constraints | 动作状态高 | 中 | 参数 sweep oracle |
| P1.3 | glue | 局部接缝中 | 低 | 几何 Gate |
| P1.4 | mask/top-left/bilinear 正确性 | 高 | 中 | render order 正确 |
| P1.5 | texture format/分辨率/display upscale | 中～高 | 负向风险 | 结构图像已正确 |
| P2.1 | motion/pose/expression/physics | 动态效果极高 | 中 | 静态 Gate |
| P2.2 | mask atlas、fixed-point、tile 并行 | 不改变语义 | 极高 | 像素 oracle 已固定 |

## 4. 分阶段实施计划

### Phase 0：冻结可复现基线和 Gate

目标：以后每一项修复都能回答“数值更接近官方了吗”，而不是只看一张照片。

工作：

1. 保留 `c0_probe --dump-runtime`，输出 canvas、deformer、blend shape target、mask set、drawable 顶点/opacity/draw order 等 host TSV。
2. 扩展官方临时 oracle，输出：
   - parameter current/default；
   - part opacity；
   - drawable position、opacity、draw order、render order；
   - multiply/screen color；
   - constant/dynamic flags。
3. 建立至少 12 组固定参数帧：
   - 默认；
   - AngleX/Y/Z 的 min/default/max；
   - EyeLOpen/EyeROpen 的 0/0.5/1；
   - mouth 0/0.5/1；
   - 至少一组 blend shape constraint 激活状态；
   - 至少一组隐藏特效/pose part 被激活状态。
4. 建立同尺寸官方 Renderer 截帧：先用 RGBA8888、640×360、黑底；再生成 1280×720 参考。
5. 仓库只保存自建脚本、schema、统计摘要和第三方材料 hash；官方二进制/模型/截图继续留在临时研究目录。

涉及文件：

- `components/otool_cubism_tool/tools/c0_probe/c0_probe.cpp`
- `components/otool_cubism_tool/spec/test_vector_schema.md`
- `components/otool_cubism_tool/test/host/`
- `docs/research_log.md`

退出 Gate：

- 一条命令可生成 self dump；
- 一条命令可比较官方/self 并输出 top-N 差异；
- 参数、drawable 和顶点数量不靠人工复制；
- 默认帧统计可稳定复现本文第 1.3 节结果。

### Phase 1：修复 P0 opacity 与 render order

目标：先恢复完整人物结构和正确遮挡关系。

#### 1A. Opacity 状态链

1. 在 runtime arena 中持久化每个 deformer 的最终累计 opacity，禁止只使用 update 函数栈上的临时数组。
2. 将 local opacity 与 parent accumulated opacity 分开保存，避免 rotation 节点当前“覆盖为累计值”、warp 节点仍保留 local 值的混合语义。
3. art mesh 最终 opacity 乘入 parent deformer accumulated opacity。
4. 预留 part opacity 输入，并明确 pose/part 与 deformer opacity 的相乘顺序。
5. `visible/enable` 作为静态/运行状态 Gate 处理，不再仅解析不使用。
6. opacity 为 0 时在进入顶点准备和 mask 之前立即跳过。

#### 1B. Render order

1. 在 self Core 中从 Draw Group/Draw Object 表计算本帧最终 render rank。
2. runtime 新增 `mesh_render_order`，类型使用整数；`mesh_draw_order` 继续作为 Core 可观察输出，不能混用。
3. Renderer 按 `mesh_render_order` 直接构造 rank→drawable 映射，不在渲染阶段猜测相同 draw order 的 tie break。
4. draw order 变化时设置动态 flag；未变化帧可复用排列。
5. 对非法重复 rank、缺 rank、越界 rank 直接返回错误，不能静默按文件顺序降级。

涉及文件：

- `src/core/self/moc3_update.hpp`
- `src/core/self/moc3_update.cpp`
- 建议新增 `src/core/self/draw_order.cpp`
- `src/renderer/model_render.cpp`
- `test/host/core_update_test.cpp`

退出 Gate：

| 项目 | 必须结果 |
|---|---:|
| 默认 opacity | 262/262 与官方绝对误差 ≤1e-6 |
| 默认可见数 | **150/262** |
| 默认 draw order | 262/262 精确一致 |
| 默认 render order | 262/262 精确一致且为 0…261 的排列 |
| 错误额外 drawable | 0 |
| host 预览 | 不再出现备用姿态/特效纹理碎片和大面积层级反转 |

Phase 1 完成后必须先给出新旧同尺寸对比图，再进入纹理升级。

### Phase 2：闭合 Core 几何与颜色语义

目标：参数变化时仍与官方一致，而不是只修默认静态帧。

工作顺序：

1. 将 update 大函数拆成参数、binding、deformer、blend shape、glue、drawable state 阶段，各阶段有独立输入/输出测试。
2. 用明确的 local/world 表示重写深层 deformer 组合：
   - rotation 保存 origin、基向量/线性部分、scale、reflect；
   - warp 保存局部 grid 与组合后的 world grid；
   - 禁止用固定 `-10/-0.1` 方向探针作为最终角度语义；
   - 对深度 1、2、5、10、15 分层建立 oracle case。
3. 实现 reflect X/Y。
4. 实现 blend shape：axis → binding → constraint → warp/art mesh/part/rotation target。
5. 实现 7 组 glue 与 322 条 vertex 对应关系，处理 intensity keyform。
6. 插值 multiply/screen color，并在 runtime 为每个 drawable 输出最终颜色。
7. 实现/验证 dynamic flags：vertex、opacity、draw order、render order、color changed。

退出 Gate：

- 默认可见顶点 150 个 drawable 全部最大误差 ≤`1e-5` model unit；
- 参数 sweep 中任一可见顶点最大屏幕误差 ≤0.25 个 640×360 像素；
- opacity 绝对误差 ≤`1e-6`；
- multiply/screen RGB 误差 ≤`1/255`；
- D67、D75～D79 不再是异常项；
- 隐藏 D231～D249 在被参数激活时也能通过几何 Gate。

### Phase 3：Renderer 像素正确性

目标：在 RGBA8888 host reference 模式先达到像素语义等价，再做 MCU 格式降级。

工作：

1. 实现标准 top-left fill rule，消除共享边重复覆盖与三角缝。
2. 增加 bilinear sampler；保留 nearest 作为性能模式，不再硬编码。
3. 以 RGBA8888 中间 framebuffer 建立 reference path，确认 PMA、Normal/Add/Multiply 的公式与官方一致。
4. 应用 drawable multiply/screen color，明确运算顺序：sample → multiply → screen → opacity/PMA → blend。
5. Mask：
   - 用 oracle 确认 mask source 是否忽略 drawable opacity；
   - 固定普通/反相语义；
   - 比较 mask alpha 图，而不只比较最终 RGB；
   - 对多个 mask source 的 union、边界、全透明 source 建单测。
6. 明确 UV wrap/clamp 和 V flip 规则；使用 atlas 边缘 fixture 防止误采样到相邻块。
7. RGB565 输出只放在 reference RGBA 路径验证之后。

涉及文件：

- `src/renderer/soft_raster.hpp/.cpp`
- `src/renderer/model_render.hpp/.cpp`
- `test/host/soft_raster_test.cpp`
- 建议新增 `test/host/image_regression_test.*`

退出 Gate：

- 纯色/PMA/三种 blend/mask/top-left/bilinear 单测全部通过；
- 官方与 self 的 640×360 RGBA reference 帧 alpha IoU ≥0.995；
- reference 帧 SSIM ≥0.99；
- 不允许存在超过 8×8 像素的非预期连通差异区域；
- RGB565/RGBA4444 设备路径单独记录量化误差，不能拿量化误差掩盖结构错误。

### Phase 4：纹理与上屏质量选择

目标：在 32 MiB PSRAM 和帧时间之间选出可量化的质量档，而不是固定使用最省内存格式。

候选：

| 档位 | Atlas | 格式 | 单张内存 | 预期用途 |
|---|---:|---|---:|---|
| Q0 | 1024² | RGBA4444 PMA | 2 MiB | 最低内存/当前格式 |
| Q1 | 1024² | RGB565 PMA + A8 | 3 MiB | 更好的颜色和 alpha，优先评估 |
| Q2 | 1024² | RGBA8888 PMA | 4 MiB | 质量基线/带宽较高 |
| Q3 | 2048² | RGBA4444 PMA | 8 MiB | 保留源 atlas 细节，需评估 PSRAM 带宽 |

工作：

1. `texture_packer` 支持上述格式并输出可验证 metadata/hash。
2. 对相同官方参考帧计算 SSIM、alpha IoU、边缘 PSNR 和内存带宽。
3. 在 640×360 下比较 nearest、bilinear、离线 mip/box-filter 三种 sampler 成本。
4. 比较三种上屏策略：
   - LVGL 2× stretch；
   - PPA/显示链缩放；
   - 直接提高内部渲染分辨率到 720×405 或 800×450。
5. 保持人物 scale/crop 与参考一致；截图比较必须同 viewport、同背景、同参数。

退出 Gate：

- 选择一个默认质量档和一个低功耗档；
- 默认档对 RGBA reference 的 SSIM ≥0.97、alpha IoU ≥0.99；
- 人脸、手指、鞋带、头发边缘没有连续破损；
- 格式选择附带实测 RAM、P50/P95 帧时间，不以主观“看起来差不多”决定。

### Phase 5：实时性、mask 优化与 WDT 关闭条件

目标：TWDT 保持开启，长时间运行不崩溃；帧率不足时可降帧但不饿死系统任务。

#### 5A. 先建立指标

每帧记录或采样：

- Core update 时间；
- render-order 构建时间；
- mask clear/raster 时间；
- drawable/triangle 候选数、实际像素覆盖数；
- texture sample/blend 时间；
- frame total P50/P95/P99；
- cooperative block 次数和最大连续 CPU slice；
- PSRAM free/minimum/high-water mark；
- dropped frame、present latency。

#### 5B. 保证调度正确

1. cooperative 触发以 `esp_timer_get_time()` 的真实时间为准，最长连续 CPU slice 目标 ≤8 ms。
2. 回调必须短暂 block，不能只用 `taskYIELD()`；当前 `vTaskDelay(1)` 方案保留并实测。
3. Cubism task 优先级不得高于 LVGL/display 必需任务；固定核策略需通过 CPU0/CPU1 两组实测决定。
4. 帧调度使用 deadline：完成后等待下一 deadline；超时则丢动画采样帧，不连续追帧。
5. 任何分辨率/质量下都必须给 IDLE task 运行窗口。
6. Debug 构建开启 `CONFIG_ESP_SYSTEM_USE_FRAME_POINTER` 以获得完整回溯；Release 不依赖该选项解决问题。

#### 5C. 降低工作量

按风险从低到高实施：

1. 先跳过最终 opacity=0 和屏外 bbox；
2. mask 只清理实际 bounding rect；
3. 将 37 个 target 合并为 16 个唯一 mask context，同一帧复用；
4. 用小型 mask atlas 或 2～4 个 A8 LRU context，避免一次分配 16 张全屏 mask（约 3.52 MiB）；
5. 预计算静态 indices/UV/screen transform 常量；
6. 将 edge equation、UV step 改为固定点增量，避免每像素 float 除法/乘法；
7. tile 化后再评估双核并行，Core update 与 raster 不并发读写同一 runtime；
8. bilinear 与高分辨率作为质量档开关，不能让低端档回退到语义错误。

退出 Gate：

| Gate | 要求 |
|---|---|
| WDT-30m | 默认动作连续 30 分钟，0 WDT、0 assert、0 内存下降趋势 |
| WDT-2h | 默认档连续 2 小时，0 WDT |
| Worst-state | blend shape/特效/mask 最重参数状态连续 30 分钟通过 |
| CPU slice | P99 ≤8 ms，最大值 ≤20 ms |
| Allocation | warm-up 后帧循环内 0 heap allocation |
| 最低性能目标 | 640×360 默认档 P95 ≤200 ms（≥5 FPS） |
| 目标性能 | 640×360 默认档 P95 ≤100 ms（10 FPS）；达不到时保留质量并明确降帧 |

如果完整正确语义在 640×360 无法达到最低性能目标，允许提供 480×270 性能档；不允许通过重新忽略 mask、blend shape、opacity 或 render order 获得帧率。

### Phase 6：恢复原版动画链

目标：从“静态 Core/Renderer 等价”升级到“同时间点播放状态等价”。

顺序：

1. pose3：part opacity 和 link part 同步；
2. motion3：曲线、fade、loop、事件；
3. expression：Add/Multiply/Overwrite 参数混合；
4. EyeBlink/LipSync group；
5. physics3：固定 timestep、输入归一化、输出权重；
6. demo 不再硬编码参数索引，使用 ID→index 映射；
7. 用固定随机种子和固定 timestep 对官方 Framework 做时间序列 oracle。

退出 Gate：

- 0、1、2、5、10 秒固定时间点的参数向量可复现；
- part opacity、drawable opacity、render order、顶点和颜色依次通过 Gate；
- motion/pose/physics 运行 30 分钟无数值发散、NaN 或 WDT；
- 最终用户对比图来自同一 motion、同一时间、同一 viewport。

## 5. 测试矩阵

### 5.1 Core 数值测试

| 类别 | 输入 | 输出比较 |
|---|---|---|
| 默认帧 | model defaults | 全部 drawable state |
| 单参数 key | min/key/default/max | key search 与插值 |
| 多轴 binding | 2～4 axes 组合 | keyform index/weight |
| 深层 deformer | 深度 1/5/10/15 | world vertex/opacity |
| Blend shape | base、单 target、constraint 边界 | target delta/color |
| Glue | intensity 0/0.5/1 | 对应 vertex continuity |
| Draw group | 大量相同 draw order | 最终 rank 排列 |
| Hidden state | parent opacity 0 | 不能进入 raster/mask |

### 5.2 Raster 单元测试

- 三角形 CW/CCW、退化、屏外、超大 bbox；
- top-left 相邻两三角形只覆盖共享边一次；
- UV 四角和 V flip；
- nearest/bilinear 的固定 2×2、4×4 texture；
- RGBA4444/RGB565+A8/RGBA8888 PMA；
- Normal/Add/Multiply；
- multiply/screen color；
- normal/inverted mask、多 source union、透明 mask；
- opacity=0 的 drawable 不触发像素循环。

### 5.3 图像回归

每个 case 保存：

- 官方 RGBA reference；
- self RGBA reference；
- embedded RGB565 输出；
- alpha diff、RGB diff、edge diff；
- SSIM、alpha IoU、最大连通差异区域；
- 参数、viewport、texture hash、代码版本。

### 5.4 真机回归

- ST7123、ILI9881C、ST7121 至少完成主 variant 和一个备用 variant；
- 30 分钟/2 小时稳定性；
- 触摸和 LVGL 同时工作时的调度；
- PSRAM 压力和显示双缓冲一致性；
- 记录 P50/P95/P99，不只记录平均 FPS。

## 6. 内存与带宽预算

当前 640×360 demo 的 Cubism 主要常驻内存约为：

| 项目 | 估算 |
|---|---:|
| moc3 对齐副本 | 0.84 MiB |
| 1024² RGBA4444 texture | 2.00 MiB |
| 640×360 RGB565 双缓冲 | 0.88 MiB |
| 640×360 A8 mask | 0.22 MiB |
| self runtime | 约 0.10 MiB |
| 小计（不含 LVGL/DSI 既有缓冲） | 约 4.04 MiB |

注意：板级显示链既有缓冲约 5.27 MiB。Q1 纹理比当前增加约 1 MiB，Q2 增加约 2 MiB，Q3 增加约 6 MiB。不能只看 32 MiB 总容量，还要测 PSRAM 随机读取带宽和与 DSI/LVGL 的竞争。

Mask 优化不建议直接缓存 16 张全屏 A8（约 3.52 MiB）。优先使用 bounding rect atlas 或少量 LRU buffer。

## 7. 建议排期与交付物

以下为单人连续研发的工程估算，不是承诺工期：

| 阶段 | 估算 | 主要交付物 |
|---|---:|---|
| Phase 0 baseline | 1～2 天 | 自动 oracle/diff、固定 case |
| Phase 1 P0 correctness | 2～4 天 | opacity/render order 全匹配、新对比图 |
| Phase 2 Core semantics | 1～2 周 | deformer/blend shape/glue/color Gate |
| Phase 3 pixel correctness | 4～7 天 | RGBA reference renderer、mask/raster Gate |
| Phase 4 quality tiers | 3～5 天 | texture/display A/B 报告、默认档 |
| Phase 5 performance/WDT | 1～2 周 | 指标、mask cache/fixed point、2h 稳定性 |
| Phase 6 animation chain | 1～2 周 | pose/motion/expression/physics 对照 |

最短可见改进路径是 Phase 1：预计先消除绝大多数当前碎片。达到“同参数静态帧接近原版”需要 Phase 0～4；达到“原版播放器动作和稳定性”需要 Phase 5～6。

## 8. 决策规则

1. **正确性优先于优化。** opacity/render order Gate 未通过前，不接受通过少画 drawable 提升 FPS 的非语义性 workaround。
2. **Core 与 Renderer 分开验收。** 顶点/state 错误不能归咎于纹理，像素错误不能用 Core 误差解释。
3. **默认帧不是完整兼容。** 只有参数 sweep 和被隐藏状态激活后仍通过，才能关闭 blend shape/glue/deformer issue。
4. **WDT 始终开启。** 允许帧率下降，不允许饿死 IDLE0/IDLE1。
5. **截图必须同条件。** 同模型、同参数/时间、同 viewport、同背景、同 atlas 才能做图像指标。
6. **质量档不能改变语义。** Q0/Q1/Q2 只允许改变采样/格式/分辨率，不允许关闭 mask、blend 或正确排序。
7. **未过 Gate 不使用“接近原版”描述。** 每一阶段都附带数值表和差异图。

## 9. 下一轮实施顺序

严格按以下顺序开始：

1. 在 runtime 中持久化 deformer accumulated opacity；
2. art mesh opacity 乘入完整父链，确认默认可见数从 217 降到 150；
3. 实现 Draw Group → render order，确认 262/262 rank 精确匹配；
4. 重新生成 640×360 host 图和 1280×720 2× 图，提交第一次结构性前后对比；
5. 再处理 D67、D75～D79、blend shape/glue；
6. 最后进入 bilinear、texture format、mask cache 和 fixed-point 优化。

只有第 1～4 步完成后，才有意义判断 RGBA4444 是否仍无法满足视觉目标。
