# 发布流程（Release Checklist）

> 每次发布/大合并前按序执行。全绿才允许打 tag。

## 1. 构建门禁

```bash
bash _tmp_build.sh          # BUILD_EXIT=0 且 error 数 = 0 (0 警告强制)
cd kernel && make kaslr-check   # 无 32 位绝对重定位残留
```

## 2. 宿主测试（WSL）

```bash
cd tests && bash ci.sh      # 七套件全绿
cd tests && bash golden.sh  # GOLDEN: PASS (与 golden.txt 逐行一致)
```

- 若输出变化为**有意变更**（如修复改变了预期），先更新 `tests/golden.txt` 并说明理由。
- 新组件测试加入 `tests/Makefile` 的显式源列表（内核 Makefile 自动 glob，测试不自动）。

## 3. QEMU 冒烟 + 长稳

```powershell
pwsh tests/net_smoke.ps1    # PASS: ping 回复 + 统计节拍 + 无异常
# 长稳 (发布前至少一次 30 分钟):
#  - 启动参数含 -net nic -net user + disk.img
#  - 判定: 0 异常/断言, stat 节拍连续, 丢=0, 速率无退化
```

## 4. 版本与发布物

- tag 格式建议 `v<year>.<month>.<n>`（与提交信息约定一致，见 docs/commit.md）。
- 发布物：ISO (`SkylineSystem-x86_64.iso`) + `disk.img` 基线 + 文档树（docs/ + SECURITY.md）。
- 发布说明必须引用本轮通过的：build log 摘要、golden 结果、soak 时长与关键数字。

## 5. 已知豁免（如实声明，不视为门禁失败）

- 零拷贝 RX（NET_ZEROCOPY=0 实验态）、收侧校验验证关闭、IRQ/MSI-X 未启用、
  调度器 pollute 相位残余方差、exit 回收路径 UAF（bench 密集退出才触发）——
  全部见 docs/goal-status.md 债务表。

## 6. 门禁矩阵速查

| 门禁 | 命令 | 判定 |
|---|---|---|
| 编译 | `bash _tmp_build.sh` | BUILD_EXIT=0, 0 错误 |
| KASLR | `make kaslr-check` | 无残留 |
| 宿主七套件 | `bash ci.sh` | 全部 failures=0 |
| 金样 | `bash golden.sh` | PASS |
| 网络冒烟 | `net_smoke.ps1` | PASS |
| 长稳 | 30min QEMU | 0 异常 + 节拍连续 |
