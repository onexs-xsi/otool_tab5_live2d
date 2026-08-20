# spec — 测试向量 schema（草案）

> 状态：draft。定义 oracle/差分协议的输入输出格式与误差规则。
> 供 `tools/oracle_runner`、`tools/diff_runner` 与 host tests 共用
> （可行性报告 §6.5）。schema 本身是本项目自有协议，不引用受限材料。

## 1. 顶层结构

每个 case 是一个 JSON 文档（也可打包为 JSONL 序列）：

```json
{
  "schema_version": 1,
  "case_id": "haru_default_min_max",
  "resources": [
    { "name": "model.moc3", "sha256": "<hex>", "source": "local-sdk-samples" }
  ],
  "moc": {
    "version_byte": 5,
    "feature_bitmap": ["warp", "rotation", "blend_shape_normal"]
  },
  "oracles": [
    { "name": "ot_core_self", "commit": "<sha>" },
    { "name": "purism_core", "commit": "<sha>" }
  ],
  "inputs": {
    "parameters": [
      { "id": "ParamAngleX", "value": 30.0, "mode": "clamp" },
      { "id": "ParamEyeLOpen", "value": 0.0, "mode": "default" }
    ],
    "seed": 20260821,
    "random_steps": 120
  },
  "expect": {
    "static_exact": true,
    "float": {
      "vertex_max_px": 0.5,
      "vertex_p99_px": 0.25,
      "opacity_color_abs": 1e-4
    }
  }
}
```

## 2. 输出记录（每个 case 一份 JSON）

```json
{
  "schema_version": 1,
  "case_id": "...",
  "runs": [
    {
      "oracle": "ot_core_self",
      "commit": "...",
      "result": "pass | fail | quarantine",
      "stages": {
        "inspect": { "canvas_w": 640, "canvas_h": 360, "counts": { "parameters": 30, "parts": 40, "drawables": 80 } },
        "memory_plan": { "total_bytes": 123456, "arena_bytes": 65536 },
        "create": { "ok": true },
        "update_steps": [
          { "step": 0, "params": [ ... ], "vertices": [ [x, y], ... ], "opacity": [ ... ], "order": [ ... ], "flags": [ ... ] }
        ]
      }
    }
  ]
}
```

## 3. 比较规则（可行性报告 §6.5）

| 类别 | 规则 |
|---|---|
| ID、count、indices、UV bits、mask、parent、texture index、order、flags | **精确一致** |
| 浮点 | 先比较 finite/符号/边界语义；映射到 640×360 后顶点 max ≤0.5 px、P99 ≤0.25 px；opacity/color 绝对误差 ≤1e-4 |
| 容差变更 | 必须改 schema 并重跑全量，禁止为通过测试而放宽 |

## 4. Case 类别

- default / min / max / 边界外参数、repeat 参数、固定 seed 随机参数向量；
- 完整 motion+physics 轨迹（由 animation 层展开为逐帧 Core 参数，避免混入 animation 差异）；
- create/update/reset flags/reload 完整生命周期；
- 畸形输入（截断、越界、溢出、错引用、cycle、NaN/Inf、未知版本/flag）→ 必须返回稳定错误码。

## 5. 存储与分发

- 可合法分发的向量进 `test/vectors/`（提交仓库）；
- 受许可限制的模型/向量只进 manifest（hash + source 引用），CI 从受控 artifact store 按 hash 取用（§6.5）。
