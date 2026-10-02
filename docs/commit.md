# 提交信息约定 (P3-82: 与 docs/release.md 的 tag 约定对齐)

Tag 格式: `v<year>.<month>.<n>` (见 docs/release.md)。提交信息按以下前缀分类,
`fix/sched` 与 `fix(driver):` 等风格均可, 但必须含**类型前缀 + 简短说明**。

| 前缀 | 含义 |
|------|------|
| `fix` | 缺陷修复 (正确性/竞态/数据丢失) —— 必须附根因与验证 |
| `feat` | 新功能/新驱动/新接口 |
| `perf` | 性能改进 —— 必须附前后数字与测量环境 |
| `refactor` | 无行为变化的结构调整 |
| `docs` | 文档/口径修正 |
| `test` | 测试/门禁 (tests/、golden、CI) |
| `hygiene` | 死代码/命名/拼写/仓库卫生 |

历史遗留的 `efix`(紧急修复)/`a`(未分类)/`smol`(小改动) 前缀不再使用;
对应提交请归入 `fix`/`refactor`/`hygiene`。
