# 研究日志（research log）— moc3/Core 自研

> 本文件记录自研 moc3/Core 兼容运行时的研究进度、难点与决策。
> 材料与工具位于临时目录 `%TEMP%/otool_cubism_research/`（不进仓库）；
> 本文档在仓库内，供实现与审计参考。

---

## 2026-08-21 — 第一轮：材料准备 + MOC3 格式逆向（头部/offset 表/CountInfo/Section 布局）

### 材料准备

拉取到 `%TEMP%/otool_cubism_research/`：

| 材料 | commit | 用途 |
|---|---|---|
| PurismCore (SakuraMotion) | `166785bb` | 结构/校验逻辑研究（C） |
| Mocari (Eatgrapes) | HEAD（depth-1） | 交叉验证（Rust parser） |
| vtubing/moc3 | HEAD | 待用（layout 交叉检查） |
| moc3ingbird (OpenL2D) | HEAD | 待用（fuzz 类别参考） |
| 本地 SDK Mao.moc3（v5） | — | 实证对象（879,680 B） |

### 已确认的 MOC3 文件布局（多源交叉验证：PurismCore + Mocari + 实证）

```
文件头（64 字节）：
  [0..4)  magic = "MOC3"
  [4]     version u8          （1..6；v5 = 5.0.00~5.2.03 导出）
  [5]     endian_flag u8      （0 = little-endian；1 = big-endian）
  [6..64) 保留（全 0）
offset 表：
  [64..64+160*4)  160 个 u32（v1~v5）；v6 为 480 个 u32
  offsets[0] = CountInfo 区；offsets[1] = CanvasInfo 区；offsets[2..] = 各 section
  要求：全部 8 字节对齐；值 ≤ 文件大小；非零 offset 单调递增（PurismCore 还要求
  section 之间不重叠，即每个 section 的 end ≤ 下一 section 的 offset）
CountInfo（版本化长度！）：
  v1~v3 = 23 个 i32（140→92 B）
  v4    = 32 个 i32
  v5    = 35 个 i32（= 140 B）
  v6    = 39 个 i32（含 offscreens/offscreen_kf/bs_offscreens + reserved）
  字段顺序（v5 的 35 个）：
  parts, deformers, warps, rotations, art_meshes, parameters,
  part_kf, warp_kf, rotation_kf, art_mesh_kf, kf_pos,
  axis_indices, bindings, axes, keys, uvs, indices, masks,
  draw_groups, draw_items, glues, glue_info, glue_kf,
  kf_mul_colors, kf_scr_colors,
  blend_axes, blend_bindings, bs_warps, bs_art_meshes,
  bs_constraint_idx, bs_constraints, bs_constraint_vals,
  bs_parts, bs_rotations, bs_glues
CanvasInfo（offsets[1]）：
  pix_per_unit f32, origin_x f32, origin_y f32, width f32, height f32, flag u8
  flag 位 0 = Y 反转标志（Y_REVERSED）
Section 表（版本化追加，index 即 offset 表下标）：
  V30 基础 101 个：count_info, canvas_info, part(id_runtime 槽,id,binding,keyform,visible,enable,parent),
    deformer(id_runtime 槽,id,binding,visible,enable,parent_part,parent_deformer,type,local_idx),
    warp(binding,keyform_off,key_count,vertex_count,row,column),
    rotation(binding,keyform_off,key_count,base_angle),
    art_mesh(id_runtime×4 槽,id,binding,keyform_off,key_count,visible,enable,parent_part,parent_deformer,
             texture_no,drawable_flag,vertex_count,uv_begin,indices_begin,indices_count,mask_begin,mask_count),
    param(id_runtime 槽,id,max,min,default,repeat,decimal_places,axis_begin,axis_count),
    part_key(draw_order), warp_key(opacity,key_pos_offset),
    rotation_key(opacity,angle,origin_x,origin_y,scale,reflect_x,reflect_y),
    art_mesh_key(opacity,draw_order,key_pos_offset),
    key_pos(xy), axis_idx(index), binding(axis_idx_begin,axis_idx_count), axis(keys_begin,keys_count),
    keys(key), uv(xy), indices(u16), mask(art_mesh_idx),
    draw_group(obj_begin,obj_count,obj_total_count,max_order,min_order),
    draw_group_obj(type,index,self_group_idx),
    glue(id_runtime 槽,id,binding,keyform_off,key_count,art_mesh_a,art_mesh_b,info_begin,info_count),
    glue_info(weight,position_idx u16), glue_key(intensity)
  V33 +1：warp quad_transform
  V42 +35：param keys_begin/count, warp/rotation/art_mesh key_color_offset, kf_mul/scr_color(r,g,b),
    param type/blend_axis_begin/blend_axis_count, blend_axis(keys_begin,keys_count,base_key_idx),
    blend_binding(axis_idx,key_bs_begin,key_bs_count,bs_constraint_idx_begin,bs_constraint_idx_count),
    bs_warp(target,binding_begin,binding_count), bs_art_mesh(同), bs_constraint_idx, bs_constraint,
    bs_constraint_val(key,weight)
  V50 +12：warp/rotation/art_mesh_key 各 +key_mul/scr_color_offset, bs_part/bs_rotation/bs_glue(target,b,b,c)
  V53 +15（v6）：part offscreen_idx, art_mesh blend_mode, offscreen(8 字段), part_key key_idx,
    offscreen_key(opacity,kmco,ksco), bs_offscreen(3)
  v5 实际使用 = 101+1+35+12 = 149 个槽位（Mao 实测 152 非零，见难点 2）
```

### Mao.moc3 实证结果（v5）

- `magic=MOC3 version=5 endian_flag=0`，160 个 offset 全部 8 字节对齐、范围合法、非零者单调递增 ✓
- CountInfo：parts=32, deformers=178 (=warps 118+rotations 60 ✓), art_meshes=262, parameters=132,
  part_kf=34, warp_kf=625, rotation_kf=248, art_mesh_kf=1359, kf_pos=143920, uvs=12040,
  indices=23829, masks=65, draw_groups=2, draw_items=263, glues=7, glue_info=322, glue_kf=7,
  kf_mul_colors=2232, kf_scr_colors=2232, blend_axes=33, blend_bindings=124, bs_warps=3,
  bs_art_meshes=31, bs_constraint_idx=234, bs_constraints=7, bs_constraint_vals=14,
  bs_parts=1, bs_rotations=3, bs_glues=0, offscreens=0
- CanvasInfo：pix_per_unit=5800, origin=(2900,4200), size=5800×8400, flag=0
- Section 抽查：part_id[0]="PartCore"、deformer_id[0]="Rotation29"、deformer_type[0]=1(rotation)、
  warp row=col=5 → vertex=36=(5+1)² ✓、art_mesh[0] vertex=62 uv_begin=0 indices_begin=0
  indices_count=273、param_id[0]="ParamAngleX" max=30 min=-30 default=0、keys=[-30,0,30,...]、
  rotation_key origin≈±0.042/scale≈0.000172（相对单位）、draw_group[0] obj 0..255、mask 65 个

### 难点记录

1. **id_runtime 指针槽位**（★ 已踩坑）：part/deformer/art_mesh/param/glue 的 id 之前、
   art_mesh 的 uv/indices/mask 之前，各有一个 `id_runtime` 等**指针占位槽**（8 字节 × count，
   文件里全 0，运行时由 Core 填充）。初版探测脚本漏掉这些槽位导致全部 section 错位，
   part_id 读出空串。**解析器必须把这些槽位当作保留槽跳过**（8 字节对齐仍满足）。
   注意：这是 64 位导出布局；若导出器为 32 位，槽宽可能为 4——需按导出器位数验证（本地产出为 8）。
2. **CountInfo 长度版本化**：23/32/35/39 words 随版本增长。PurismCore 用固定 39 字段结构读取
   所有版本（v5 时后 4 个字段越界读到相邻数据，恰好为 0 未触发校验——**实现缺陷**，不能模仿）；
   Mocari 只解析前 25 个字段（跳过 blend shape 字段）。**自研实现按版本读取正确 word count，
   未知长度拒绝**。
3. **deformer type 枚举**：0=WARP, 1=ROTATION（PurismCore deformer.h 确认）。
4. **endian_flag 语义**：0 = 文件为 LE（本机 LE 时无需转换）；1 = BE。读取所有多字节字段
   前必须按此转换；仅支持 LE 时遇到 flag=1 直接拒绝（MVP 白名单）。
5. **Mao 画布 5800×8400 非 16:9**（≈1:1.448 竖版）：640×360 目标渲染需要 letterbox 或裁剪策略
   ——模型 profile 冻结时必须确认生产模型画布比例与渲染目标的关系。
6. **PurismCore 的 psm__verify_count_info 只校验部分字段**（parts..glue_kf 等 23 个 +
   warps+rotations==deformers）：自研 validator 必须全字段校验（负值、超出 hard limits、
   warps+rotations==deformers、monotonic、不重叠、对齐）。
7. **V50 keyform 颜色区数据模式异常**（★ 待 oracle 对照）：
   - `warp_key.key_mul_color_offset` 读出 [1065353216, 0, 1065353216, 0, ...]（= f32 1.0/0.0 交替），
     而同类的 `art_mesh_key.key_mul_color_offset` = [0,1,2,3,...] 递增整数；
   - `kf_mul_color_r` 前几值为递增小整数（≈0x33D、0x33E…，f32 读法 ≈1.16e-42）；
   - 可能为 v5 的颜色量化/打包格式（RGBA8 或标志位编码）。C0 parser 按 u32/f32 原样读取，
     语义解释留到 C2/C5 层并用官方 Core oracle 对照确认。
8. **count=0 的 section 的 offset 语义**：可为 0（未使用）或指向任意合法位置（Mao bs_glue
   count=0 时最后两个槽位 offset 相同 0xd6c40）；validator 对 count=0 不应要求 offset 唯一。

### Ren.moc3（v6）初步实证

- `magic=MOC3 version=6`，**480 个 offset**（v6 布局），非零 167 个，全部对齐/范围内/单调 ✓
- CountInfo：parts=51, deformers=150 (=warps 128+rotations 22 ✓), art_meshes=198,
  parameters=73, part_kf=54, warp_kf=848, rotation_kf=97, art_mesh_kf=834, kf_pos=154688,
  axis_indices=103, bindings=76, axes=65, keys=422, uvs=15686, indices=33123, ...（截断，后续补全）
- v6 的 V53 sections（offscreen 等 15 个）与 480 槽位机制待细化（167 非零 vs v5 149+15=164 的
  差值 3 待解释）
- v6 不进入首版（MVP profile = v5），但 parser 必须**识别并拒绝** v6（报告 §6.2）

### 下一步

1. 完成 Mao 剩余 section（86~151）验证与 glue/blend shape 字段语义确认。
2. 交叉验证 Ren.moc3（v6）确认 v6 布局差异（480 offset、CountInfo 39 words、V53 sections）。
3. 依据本日志编写 `spec/format/moc3_profile_v5.md` 冻结版（G-FMT 输入）。
4. 设计并实现自研 C0：bounded reader → validator → 不可变 IR → memory plan。

---

## 2026-08-21 — 第二轮：C0 安全解析器实现与验证

### 已交付（全部自研，未复制第三方代码）

| 文件 | 内容 |
|---|---|
| `src/core/self/moc3_common.hpp` | 版本常量、CountInfo 版本化 word count（23/32/35/39）、offset 槽位数（160/480）、model_info_t |
| `src/core/self/moc3_reader.hpp` | 有界读取器：range/对齐检查、checked add/mul、LE/BE 字节序、错误上下文（err_info） |
| `src/core/self/moc3_validate.hpp/.cpp` | C0 inspect：magic/version/endian/offset 表（对齐+单调）/CountInfo（版本化）/CanvasInfo（NaN 拒绝） |
| `tools/c0_probe/c0_probe.cpp` | PC host 探针（MSVC 编译，与固件共用同一源码） |

### 验证结果（host 实测）

| 用例 | 结果 |
|---|---|
| Mao.moc3（v5，879,680 B） | **OK**；section_count=152、CountInfo 全部与 Python 探测一致、canvas=5800×8400 |
| Ren.moc3（v6） | **PROFILE_MISMATCH**（v5 profile 门禁正确拒绝 v6）✓ |
| Haru/Wanko（v1）、Hiyori/Mark/Rice（v3） | **PROFILE_MISMATCH** ✓ |
| 100 字节截断文件 | **TRUNCATED** + 稳定错误上下文（section=0xFFFF offset=0x2c0）✓ |
| 坏 magic（XYZ Z） | **BAD_MAGIC** ✓ |
| endian_flag=1（BE） | **UNSUPPORTED_ENDIAN**（MVP 明确拒绝而非转换）✓ |

### 实现决策记录

1. **section 单调性**：PurismCore 要求"section 之间不重叠"（prev_end 跟踪）；MVP 采用**非零 offset 严格单调**（等价于不重叠，且实现更简单）。
2. **CountInfo 版本化读取**：按版本读 23/32/35/39 words，未用字段强制清零——规避 PurismCore 固定 39 字段越界读的缺陷（研究日志难点 2）。
3. **错误上下文**：所有失败路径返回 err_code + section + byte offset + index（spec/error_codes.md 实现）。
4. **profile 门禁先收紧**：inspect 只接受 v5（G-FMT 冻结前）；v1/v3/v6 一律 PROFILE_MISMATCH，与 spec/hard_limits 一致。
5. **C0 编译单位**：固件 REALTIME(SELF) 配置编译；clip-only 配置不含 core 源码（CMake 条件源文件）。

### 难点（新增）

9. **host 编译器**：本机无 x86 g++/clang++，MSVC Build Tools 位于
   `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`；vcvarsall 环境
   需 `cmd /c "call vcvarsall.bat x64 && ..."` 包装才能在 PowerShell 使用。
10. **C4819 警告**：MSVC 对含中文注释的 UTF-8 源码报警（代码页 936）；不影响构建，
    后续可统一加 `/utf-8` 或改英文注释。
11. **powerShell 数组索引赋值**不可用（`$b[0..3]=...` 报错），改写需
    `[Array]::Copy` + 新数组。

### 下一步

1. C1：静态模型 IR —— section 布局表（研究日志第一轮结论）→ 不可变 typed IR + 全部静态数组校验
   （UV/indices/mask/parent/texture/constant flags 精确匹配）。
2. memory plan（`ot_core_query_memory`）：一次分配布局计算。
3. host 负例 fuzz 语料（截断点/溢出/错引用/cycle）。
4. 完成 v6（Ren）布局细节记录（480 槽位差异）。

---

## 2026-08-21 — 第三轮：C1 不可变 IR + 显示链路方案

### C1 交付（自研）

| 文件 | 内容 |
|---|---|
| `src/core/self/moc3_ir.hpp/.cpp` | 152 项 section 规格表（V30 101 + V33 1 + V42 35 + V50 15）+ typed 视图解析 + 全量静态引用校验（part/deformer/art_mesh 父链、binding/axis/keys、keyform 组合 2^axis、warp 网格、UV/indices/mask、glue、draw group、blend shape） |
| `tools/c0_probe` 扩展 | 增加 IR 构建与抽查输出 |

**host 验证（Mao.moc3）**：`ir build -> OK`，slots=152；art_mesh[0] vc=62/uv_begin=0/idx 273/kf 0/1；
uv[0..3]、indices[0..5]、param[0]（max30/min-30/default0）、kf_pos[0..3] 与 Python 探测**逐值一致**。

### 难点（新增）

12. **V42 规格表错位**：`param_ext.key_runtime`（指针槽）最初漏掉，导致 102 之后全部错位 1
    （V50 实际从 137 起 15 项）。修正后 static_assert(152) 通过。教训：**每个版本的 section
    规格必须与 CountInfo 版本化机制一致核对**（V42 字段在 v4.2 才存在）。
13. **初步显示路径决策**：不先做 display lease（需改 otool_lvgl_idf_port 子模块），
    改为 **cubism 渲染 640×360 → LVGL image（2x 缩放）显示**——先验证渲染正确性，
    独占租约后置为优化项。
14. **纹理路径**：设备端不解析 PNG；PC 端 texture_packer（Pillow）转换
    PNG → 预乘 alpha RGBA4444 raw（1024² = 2 MiB），设备直接加载。

### 下一步

1. 渲染器最小实现：三角形光栅 + RGBA4444 纹理采样（nearest）+ alpha 混合。
2. 最小 update：参数默认值 → keyform 选择 → art_mesh 顶点（kf_pos）→ 渲染。
3. LVGL image 集成（main）：640×360 帧缓冲 → 2x 上屏。
4. 纹理与 moc3 素材进 SD/flash 的加载路径。

---

## 2026-08-21 — 第五轮：C2+C4 update 实现、软光栅、渲染管线打通

### 交付（自研）

| 文件 | 内容 |
|---|---|
| `src/core/self/moc3_update.hpp/.cpp` | C2（参数 clamp → 轴 key search → combo builder 2^N 组合）+ C4（warp 双线性/三角形插值、rotation 仿射、嵌套链深度排序、rotation 父链合成：origin 经父变换 + 角度数值探测 + scale/opacity 累积） |
| `src/renderer/soft_raster.hpp/.cpp` | 最小软光栅：bounding-box + 重心、RGBA4444 nearest、预乘 alpha 混合 |
| `src/renderer/model_render.hpp/.cpp` | IR+runtime → 画布→屏幕 fit 变换 → 逐 drawable 渲染（opacity、UV 翻转） |
| c0_probe 扩展 | 完整管线 host 验证：create → update → render → PPM |

### 关键 bug 修复（难点记录）

15. **key_pos_offset 是 f32 索引**（与 uv_begin 同语义，非顶点对索引）——`&pos_pool[po*2]` 错误导致 am[96+] 越界读垃圾值 → 崩溃。修复为 `&pos_pool[po]`；C1 校验同步改为 `range_ok(po, vc*2, n_kf_pos)`（PurismCore 只查 `po < kf_pos`，宽松，不模仿）。
16. **嵌套 deformer 必须深度序处理**（Mao 173/178 个 deformer 有父，链深达 14）。
17. **rotation 父链合成**：origin 经父变换、angle 加父角度（数值探测：10 次迭代、step 依赖父类型 ±10°/±0.1）、scale × 父 scale 累积（PurismCore 与 Mocari 算法一致，交叉验证 ✓）。
18. **坐标语义（待 oracle 确认）**：Mao 的 rotation scale=0.0001724 ≈ 1/5800（画布宽倒数），推测 rotation 把**画布坐标映射到父 warp 的归一化空间**；挂 rotation 的 art_mesh 局部顶点是画布坐标，挂 warp 的是归一化。**当前实现 render lit=0**（rotation 链顶点落点错误），需要官方 Core oracle 对照确认语义后修正。
19. **工具链纪律**：PowerShell Get-Content/Set-Content 默认 GBK，会破坏 UTF-8 中文源文件（两次事故，一次不可逆）；**组件源码注释改为英文 + 只使用 edit/write 工具修改**。

### 当前状态

- 无崩溃、无 inf 顶点、217/262 drawable 可见（opacity>0）
- 渲染输出 lit=0：rotation 父链的坐标语义问题（难点 18）
- 下一步：官方 Core（Windows x64 静态库）作为 oracle 跑 Mao，导出顶点对照 → 修正坐标语义 → 正确渲染



