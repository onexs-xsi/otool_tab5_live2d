# test/vectors — 测试向量清单

> 存储与分发规则见 `spec/test_vector_schema.md` §5 与可行性报告 §6.5：
> - 可合法分发的向量提交本目录；
> - 受许可限制的模型/向量**只登记 manifest**（hash + source），
>   不把二进制放进仓库；CI 从受控 artifact store 按 hash 取用。
>
> 当前状态：**空**。G-FMT 冻结与最小自建 fixture 建立后开始填充。

## manifest.yml（登记格式）

```yaml
schema_version: 1
vectors:
  - case_id: <case_id>
    file: <提交仓库的文件名，无则 null>
    artifact_ref: <受控 store 引用，无则 null>
    sha256: <hex>
    source: <local-sdk-samples | self-built-fixture | production-model>
```
