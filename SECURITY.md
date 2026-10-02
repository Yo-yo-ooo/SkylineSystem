# 安全现状与漏洞流程（SECURITY）

> 如实声明：本 OS 是学习型单用户内核，安全机制覆盖"架构级"而非"策略级"。
> 报告问题请见文末流程。

## 已实现（架构级）

| 机制 | 位置 | 状态 |
|---|---|---|
| 内核/用户地址空间隔离 | VMM | ✅ |
| SMEP / SMAP | `enable_smep_smap()` | ✅ |
| NX (EFER.NXE) | 按 CPUID 启用 | ✅ |
| 用户态 W^X | ELF 装载按 p_flags 收紧 | ✅ |
| KASLR | 引导器装载随机化 (`make kaslr-check` 门禁) | ✅ |
| 用户指针校验 | `ua.cpp` MM_USER 校验 + 半区上界 (曾修复 HHDM 别名漏洞) | ✅ |
| syscall 入口校验 | 越界/未注册 → -ENOSYS | ✅ (round 9 审计) |
| 敏感操作信任位 | kill/exec/跨进程 mmap 需 `IsTrusted` | ✅ |
| 内核栈 | 16KB 默认, 深调用链组件可 32KB | ✅ (round 14) |

## 明确边界（非缺陷，是设计现状）

- **无 uid/gid、无文件权限位、无 chmod/access**：单用户 OS，敏感操作走进程信任位。
- **用户态 ASLR 内核侧已实现**：ELD 装载器支持 ET_DYN load bias（随机基址）；
  树内程序当前按静态基址编译未启用（P3-79 口径修正：原写"无用户态 ASLR"）。
- **无安全模块/CVE 编号体系**：本 OS 不分配 CVE；上游 lwIP/lwext4/FatFs 的 CVE
  需跟踪（见下）。

## 已知风险（技术债清单）

1. **内核 `_vsnprintf` 加固待办**：`%s` NULL 守卫已加；lwIP 多模块调试流下的
   崩溃已归因于线程栈（round 14 修复），但 vsnprintf 的长格式/嵌套场景未做穷举。
2. **零拷贝 RX = 实验态**（`NET_ZEROCOPY=0` 默认关闭），见 docs/dpdk-lite.md。
3. **sched_bench 真内核未验证**（模型测试通过）。
4. 上游组件版本跟踪：lwIP / lwext4 / FatFs 的 CVE 公告需定期比对
   `kernel/src/net` 与 `kernel/src/fs` 的 vendored 版本。

## 漏洞报告流程

1. 在项目仓库开 Issue，标题带 `[SECURITY]`，附复现步骤（QEMU 参数 + 日志）。
2. 涉及上游（lwIP/lwext4/FatFs）的问题会转报上游并记录转报编号。
3. 修复合入后更新本文档的"已知风险"与 docs/stability-audit.md。

## 上游 CVE 跟踪清单（C6, round 15）

> 无自动化时的最低开销人工跟踪：每季度逐行核对以下版本与 NVD 公告。
> 自动化方向（可选）：GitHub Action 定时拉取 NVD 的 cpeMatch，匹配
> 本表 pin 的版本，命中则开 Issue。

| 组件 | vendored 位置 | pin 版本 | 最近核对 | 待办 |
|---|---|---|---|---|
| lwIP | kernel/src/net | 仓库内置（未记录上游 tag） | round 15 起 | 记录上游 tag |
| lwext4 | kernel/src/fs/lwext4 | 仓库内置 | round 15 起 | 记录上游 tag |
| FatFs | kernel/src/fs/fatfs | 仓库内置 | round 15 起 | 记录上游 tag |
| mpaland/printf | lib/stdc/outfb/printf.c | 上游 MIT 版 | round 15 起 | 跟踪上游修复 |
| x86mem | ablib/arch/x86_64/x86mem | 本仓库自研（MIT） | — | 无 |
