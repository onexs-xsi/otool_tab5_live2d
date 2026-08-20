# spec/behavior — C0..C6 行为契约（骨架）

> 状态：全部 draft。每层有独立输入/输出向量并通过 Gate 后才能进入下一层
> （可行性报告 §6.3 分层表）。本骨架只声明各层**可观察输出**与契约要点；
> 具体字段、公式与容差在 G-FMT/G-BHV 冻结时按批准来源填写。

## C0 Header / Profile

- 可观察输出：magic/version/endian/canvas、计数与预算、拒绝原因、memory plan。
- 契约要点：版本字节 ≠5 拒绝；计数满足 hard limits；未知 flag 拒绝。
- 向量：default / 截断 / 溢出 / 未知版本 / 未知 flag / NaN canvas。

## C1 Static Model

- 可观察输出：IDs、UV、indices、texture index、mask、parent、constant flags。
- 契约要点：与批准参考**静态数组精确一致**；ID 转组件 ID 表。
- 向量：每表全量 dump + 错引用/越界拒绝路径。

## C2 Parameters

- 可观察输出：default/min/max、clamp/repeat、key search 区间与权重、参数 runtime 值。
- 契约要点：clamp 与 repeat 语义、key 单调性、绑定组合。
- 向量：default / min / max / 边界外 / repeat / 非单调 key。

## C3 Interpolation

- 可观察输出：多参数 keyform 权重、边界与外推行为、混合结果。
- 契约要点：权重归一化、端点语义、外推规则。
- 向量：单/多参数、端点、外推、NaN 拒绝。

## C4 Deformer Graph

- 可观察输出：每级 deformer 输出点与 opacity；warp/rotation/reflection/scale、父链。
- 契约要点：拓扑序更新、cycle 拒绝、最大深度。
- 向量：链式/树形图、深度边界、cycle 拒绝。

## C5 Drawable State

- 可观察输出：顶点、draw/render order、可见性、opacity、color、动态 flags。
- 契约要点：part/artmesh/glue 组合、order 语义、flags 生命周期与 reset。
- 向量：完整模型 dump + flags 生命周期。

## C6 Blend Shape

- 可观察输出：各层增量与最终 drawable；constraint。
- 契约要点：part/warp/rotation/artmesh blend shape 语义。
- 向量：单/多 keyform、约束链。
