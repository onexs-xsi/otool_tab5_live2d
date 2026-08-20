# spec/format — moc3 profile v5（骨架）

> 状态：draft（G-FMT 输入冻结前不填充字段级布局）。
>
> 本文件只声明 **目标 profile 与校验规则类别**。字段级二进制布局
> （section 布局、offset/count 关系、元素尺寸、对齐要求）必须：
> 1. 依据研究材料整理——允许逆向任何文件（官方 Core 二进制、样例 moc3、
>    参考实现源码等），材料放临时文件夹（%TEMP%/otool_cubism_research/）；
> 2. 逐字段标注来源；
> 3. 冻结后由 asset_validator 与固件 parser 共享同一份 schema 定义
>    （各自独立实现，避免同一 bug 双重通过，可行性报告 §6.2）。

## 1. 目标声明

| 项 | 值 |
|---|---|
| moc3 版本字节 | 5（SDK 5.0.00～5.2.03 导出版本范围） |
| 字节序 | 待冻结（首个 u32 版本字段在冻结时确认） |
| magic | 4 字节，冻结时确认常量值 |
| 端到端 | 版本字节 ≠5 → 拒绝；未知 flag → 拒绝；manifest 版本不匹配 → 拒绝 |

## 2. 校验规则类别（每类在冻结时给出具体字段与算法）

1. 边界：所有 `offset + count × element_size` checked arithmetic，
   解引用前验证（§6.2 最低校验集）。
2. 对齐：section 对齐要求，不满足即拒绝。
3. 重叠：section 不得意外重叠、指向 header/table 或 wraparound。
4. 引用完整性：index/parent/mask/binding 全部 referential-integrity 检查。
5. 拓扑：deformer/part 依赖图可拓扑排序，最大深度 ≤16（hard_limits.md）。
6. 数值：拒绝 NaN/Inf；有序 key 序列验证单调性与重复键。
7. 计数关系：count 满足格式关系与 hard limits 双重约束。
8. 字符串：ID 有界（≤128），转换组件 ID 表，不在 blob 外找终止符。

## 3. Feature 清单（按可观察行为分层）

见 `../behavior/` 下 C0～C6 契约骨架。首版完成面 = 生产模型实际使用的子集；
未使用的 feature 必须被明确识别并拒绝（§6.3），不得无声忽略。

## 4. 冻结流程

1. 选择生产模型并重导出（版本字节 5）→ corpus_tool 记录统计。
2. 逆向/研究材料整理后逐字段填写本文件 → 冻结为 v1（spec_version 标记 frozen）。
3. asset_validator 与固件 parser 按冻结版实现，双方跑同一组向量。
