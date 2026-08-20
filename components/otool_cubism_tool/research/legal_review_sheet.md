# G-LGL：法律/来源审查单（一页式模板）

> 可行性报告 §6.6 G-LGL 与 §10.1 L0。**Core 格式/行为编码的前置条件**。
> 由项目负责人/法务填写并签字；未关闭前不写 moc3 格式/行为实现代码。

## 1. 基础信息

| 项 | 值 |
|---|---|
| 项目 | otool_tab5_live2d（components/otool_cubism_tool） |
| 填表人 / 日期 | |
| 审批人 / 日期 | |
| 审批 ticket | |

## 2. 研究方法（二选一）

- [ ] **Reference-assisted（本工作区现状）**：实现者可接触本仓库内 SDK、
      已批准的 GitHub 参考项目与合法模型；每个来源按 `research/reference_manifest.yml` 记录。
- [ ] **Strict clean-room**：实现团队只接收法务批准的行为规格与测试向量；
      需要隔离仓库、隔离团队与访问控制（选择此项则暂停本工作区 Core 实现）。

## 3. 可用来源（逐项书面确认）

| 来源 | 允许用途（code-reference / oracle / test-only / excluded） | 批准人 |
|---|---|---|
| Live2D Cubism SDK for Native 5-r.5（third_party 本地副本） | | |
| 官方 Core 作为差分 oracle（自动化比对） | | |
| SakuraMotion/PurismCore（固定 commit） | | |
| Eatgrapes/Mocari（固定 commit） | | |
| vtubing/moc3 | | |
| moc3ingbird（含 PoC，隔离运行） | | |
| py-moc3（反编译链路，隔离） | | |
| moc3-reader-re（排除） | | |
| 其他（请在参考清单补充） | | |

## 4. 模型与输出

| 项 | 结论 |
|---|---|
| 生产模型（名称 / 导出工具 / 版本字节 5 可重导出？） | |
| 官方示例模型可否用于本地测试 / 提交到仓库 / CI artifact？ | |
| 测试输出（截图 / 差分数据 / 测试向量）可否提交仓库？ | |
| 发布方式（固件 / 素材包 / 文档）的许可边界 | |

## 5. 对外命名与商标

- [ ] 对外命名为 "otool 独立 moc3-compatible runtime"，不声称官方 Cubism Core、
      Live2D 背书或隶属关系（可行性报告 §3.4）。

## 6. Framework 复用（如需要）

- [ ] 不复用（组件自有 animation backend，默认）
- [ ] 复用官方 Framework（需 Live2D Open Software License 审查 + 受控导入
      `vendor/live2d_framework` + `csm` shim 审批）

## 7. 结论

- [ ] **G-LGL 通过**（所有项有书面结论，无"默认允许"假设）
- [ ] G-LGL 有条件通过：条件为 ____________________
- [ ] G-LGL 未通过：____________________

签字（负责人）：________   签字（法务/合规，如适用）：________
