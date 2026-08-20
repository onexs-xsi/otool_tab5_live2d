# otool_cubism_tool — 内部规格（spec/）

> 本节是版本化的内部规格，**不含受限代码**。它定义"otool 独立
> moc3-compatible runtime"的目标 profile、校验规则与行为契约，
> 供实现、测试向量和 PC oracle 共用。字段级二进制布局只有在
> G-FMT 冻结后按批准来源补充，禁止凭未批准材料臆造。

## 目录

```
spec/
├── README.md                 # 本文档
├── hard_limits.md            # 产品硬上限与内存预算（草案）
├── error_codes.md            # ot_core_error_t 错误码体系（草案）
├── test_vector_schema.md     # oracle 输入/输出格式与误差规则
├── format/
│   ├── moc3_profile_v5.md    # 目标 profile：版本字节 5 的字段/校验规则（骨架）
│   └── package_manifest.md   # 素材包 manifest 草案（S2 与 clip_packer 共用）
└── behavior/
    ├── c0_header_profile.md  # C0：header/profile 行为契约（骨架）
    ├── c1_static_model.md    # C1：静态模型数据契约（骨架）
    ├── c2_parameters.md      # C2：参数 clamp/repeat/key search（骨架）
    ├── c3_interpolation.md   # C3：多参数 keyform 插值（骨架）
    ├── c4_deformer_graph.md  # C4：warp/rotation 形变图（骨架）
    ├── c5_drawable_state.md  # C5：part/artmesh/order/flags（骨架）
    └── c6_blend_shape.md     # C6：blend shape 与 constraint（骨架）
```

## 规格生命周期

| 状态 | 含义 |
|---|---|
| draft | 骨架/草案，未冻结；不可作为实现依据 |
| frozen | 已通过对应 Gate（G-FMT / G-BHV 子集），实现与测试必须以 frozen 版本为准 |
| superseded | 被新版本替代；保留历史，实现必须迁移 |

规格变更必须：更新 `spec_version`、追加 changelog、重新跑关联向量。

## 当前状态

- 全部文件：**draft**（2026-08-21 建立）
- 阻塞项：G-LGL（research/legal_review_sheet.md）与生产模型冻结（G-FMT 输入）
