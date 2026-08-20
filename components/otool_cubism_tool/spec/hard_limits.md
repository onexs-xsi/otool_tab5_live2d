# spec — 产品硬上限与内存预算（草案）

> 状态：draft（待 G-FMT 与生产模型 profile 冻结）。
> 所有值是可调草案；冻结后由 `generated/core_feature_manifest.h` 固化，
> parser/validator 与 asset_validator 共用同一份值。

## 1. 模型规模硬上限（草案）

| 类别 | 上限 | 依据/备注 |
|---|---:|---|
| moc3 版本字节 | 仅 5（首版） | 版本 1/3 仅回归样例；6 需独立 Gate |
| 画布 | 640×360 目标（16:9） | 渲染后 PPA 2× 上屏 |
| 纹理数 | 2 | 离线缩至 ≤1024² |
| 纹理尺寸 | 1024×1024 | RGBA4444 预乘 alpha；单张 1 MiB |
| Parameters | 256 | |
| ParameterGroups | 64 | |
| Parts | 128 | |
| Deformers（总） | 128 | 含 warp/rotation/reflection/scale 与父链 |
| Deformer 链深度 | 16 | 拓扑排序 + 最大深度 |
| Drawables | 256 | |
| 顶点总数 | 65536 | |
| 索引总数 | 131072 | u32 索引 |
| 每 Drawable 三角形 | 4096 | |
| Keyforms（总） | 1024 | |
| Masks / ClippingContext | 64 | A8 atlas 512² |
| Blend modes | Normal / Add / Multiply | 扩展 blend 禁用 |
| Offscreen（5.3） | 0 | 首版禁止 |
| 每参数绑定 | 16 | |
| ID 字符串长度 | 128 | 转组件 ID 表，不在 blob 内找终止符 |

## 2. 实时模式内存预算（草案，可行性报告 §5.2）

| 项 | 预算 | 说明 |
|---|---:|---|
| 640×360 RGB565 场景缓冲 ×2 | 0.88 MiB | |
| 1024² RGBA4444 纹理 ×2 | 2.00 MiB | 预乘 alpha |
| 512² A8 mask atlas | 0.25 MiB | |
| moc3 blob（只读） | ≤1 MiB | 带长度，offset 不改写指针 |
| 验证后 IR/索引 | ≤1 MiB | typed view + 拓扑序 |
| self Core runtime arena | ≤1 MiB | 由 `ot_core_query_memory()` 计算 |
| 安全余量 | ≥6 MiB 或 PSRAM 20% | 取较大者 |

## 3. 运行时行为上限

| 项 | 上限 |
|---|---:|
| update 每帧分配 | 0（热路径无 malloc/free） |
| 参数更新批次 | ≤64 条/帧 |
| 输入队列深度 | 8（config 可调） |
| 帧间隔 | 33333 µs（30 FPS 目标） |

## 4. 超限策略

- 离线：`asset_validator` 直接报错，不生成包。
- 设备端：manifest/profile 校验在**任何大块分配前**拒绝加载（可行性报告 §4.6）。
- 未知 feature：parser 必须明确识别并拒绝，不得无声忽略（§6.3 C0 层）。
