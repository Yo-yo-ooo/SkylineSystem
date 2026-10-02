# 目标完成度总账（round 27 汇总，round 88 里程碑更新）

> 目标三部分：(A) e1000 驱动 + (B) lwIP 移植完善 + (C) DPDK 式高性能改造，
> 外加生产级大清单（SMP 并发/长期稳定/故障注入/安全/完整语义/性能画像/
> 回归门禁/可观测性）。逐项对照，如实标注。

## 🏁 里程碑：四源审查 110 项修复清单 —— ✅ 全部清账（round 48–88）

六档全清：**P0 ×18**（含两项回退重做收敛：P0-2 TLB ACK fence——TCG 下
4 亿 pause 自旋根因→100ms 墙钟封顶，实测 0 超时；P0-12 AHCI 每请求
DMA 缓冲替代跨 sti 窗口锁）、**P1 ×36**、**P2 ×8**（P2-55 跨核 ABA 隔离栈
foreign_head、P2-59 AHCI 释放屏障、P2-61 QSBR 64 槽）、**P3 ×21**、
**P4 ×8**（allocator 宿主混沌测试——还抓到 MCB 分配粒度真 bug；fc 写回
内容校验）、**P5 ×19**。每轮验证：BUILD_EXIT=0 + GOLDEN PASS + QEMU
ping 3 + 0 异常。

## 🏁 路线图收敛（round 90–100）

| # | 路线图项 | 状态 |
|---|---|---|
| #7 | FD 继承 (filedesc 快照式深拷贝) | ✅ round 90 —— 顺带实锤两个潜伏缺陷 (rb_postorder_iter 回调契约 + exec FDMan 释放不置空) |
| #2 | proc 生命周期 TOCTOU | ✅ round 91 —— ProcessAddThread 锁内重检 + 收割器 FDMan 回收入锁 (5 调用点闭环) |
| #6 | vsnprintf fuzz | ✅ round 92-93 —— ASAN 宿主 fuzz + 真越界读修复 (NUL 终结格式串) |
| — | fsync 生产激活 | ✅ round 94 —— fclose + 进程退出隐式冲刷脏页 |
| P2-62 | Sleep/timer 轮接活 | ✅ round 96 —— PIT::Sleep 双模 (调度器就绪→真睡眠) |
| — | RX burst 可观测 | ✅ round 97 —— rx_burst_avg (DPDK 口径) |
| — | 静态分析 | ✅ round 98-99 —— clang --analyze 9 文件 + 2 防御修复 (fc LRU 双步进 + rbtree sibling 守卫) |

## A. e1000 驱动 —— ✅ 完成

- 82574L (QEMU e1000e) 完整驱动：探测/复位/EEPROM MAC/双环 256/中断+轮询双模
- 五轮真 bug 修复史：RDT 所有权、_memcpy 转置序、GetPhysics 页内偏移、
  pbuf 双释放、探测期 PIT::Sleep 挂死、早期 PIT 忙等守卫
- 多核 TX 自旋锁串行化 + 环满退避

## B. lwIP 移植 —— ✅ 完成（生产默认：轮询+拷贝路径）

- 端到端验证：DHCP 租约 (10.0.2.15)、ICMP ping 连续回复、多核 TX 洪泛 2.2-2.7k 帧/s
- 移植修正：lwipopts（软件校验/对齐/池）、cc.h DIAG varargs 链修复、
  sys_now 单调毫秒、ACD 关闭（slirp ARP 代理）、ip4addr_ntoa 静态缓冲坑
- 网络线程栈 32KB（16KB 栈溢出 = 全部间歇性崩溃的深层根因之一）

## C. DPDK-lite —— 🟡 基座完成，两实验项文档化

| DPDK 概念 | 状态 |
|---|---|
| PMD 轮询 | ✅ 生产路径 |
| 多核 TX 并发 | ✅ 自旋锁 + 3 核洪泛验证 |
| 每核私有/无锁 | 🟡 单 RX 队列（双队列 = MSI-X 阻塞项） |
| 零拷贝 RX | 🟡 实验态（NET_ZEROCOPY=0）：已修索引反推+双释放，仍存首 stat 后静默（嫌疑收窄至 2 项，见 dpdk-lite.md） |
| 校验卸载 / MSI-X | ❌ 寄存器手册核对阻塞 |
| 统计/可观测 | ✅ 计数器 + RTT P50/P90/P99 + 停滞自诊断 |

## 生产级大清单逐项

| 支柱 | 状态 | 证据 |
|---|---|---|
| SMP 并发正确性 | ✅ 既有（SLUB/FC/sched 多线程套件）+ 网络多核压力 + **110 项 P0/P2 并发修复全清** | tests/ + soak |
| 长期稳定性 | ✅(证据) 收官 soak 链: 10/15/30min + 40min 多段全部 0 断言 0 异常 0 stall; 最终 30min: tx=129 万帧(82.7MB) 2429/s 持续, ping=185 连续, 丢=0 (round 37 收割); **round 89 起按里程碑复测** | serial.log |
| 故障注入 | ✅ OOM/页分配/磁盘错误(round 22 新测)/RX OOM 注入全闭环 | fc_reg + net 注入 |
| 安全审计 | 🟡 syscall 入口审计 ✅、SECURITY.md ✅、信任位模型文档化；fuzzing/纵深 ❌ | SECURITY.md |
| 完整语义 | ✅ writeback/脏页/fsync/OOM 语义审计（fc-semantics.md）；权限=信任位（如实） | fc-semantics.md |
| 性能画像 | ✅ RTT 百分位 + sched_bench 真内核报告（step/osc 过；pollute=真发现、shortwin=语义失配） | scheduler.md |
| 回归门禁 | ✅ 三级门禁：宿主 CI + 金样 diff + QEMU 网络冒烟 | tests/ |
| 可观测性 | ✅ 计数器/速率/RTT/panic 进串口/停滞自诊断 | 各 round 验证 |

## 遗留技术债（按优先级）

| # | 债 | 状态 | 修复设计 |
|---|---|---|---|
| 1 | pollute 相位 RIP 倍率残余方差 | 3x 收敛已获 (累加器), 完整通过待窗口内插桩 | 分子侧/相位过渡插桩 |
| 2 | exit 回收路径自旋锁 UAF | 符号化定位; round 40-41 已修根因, 生产 soak 未触发; **P1-23 口径: 守卫是 check-then-act 缓解而非根治, 僵尸回收仍无全局同步** | proc 引用计数或 FDMan 延后释放 (根治) |
| 3 | 零拷贝剩余嫌疑 | 见 dpdk-lite.md (嫌疑收窄至 2 项) | pbuf 生命周期运行时跟踪 |
| 4 | sched_bench shortwin 采样 | round 47 已破案; 剩余: **切换点采样实施** (round 47b 结论) | switch-out 采样 + tick 去重 |
| 5 | 校验卸载 / MSI-X 多队列 | 82574 数据手册阻塞 | 寄存器核对后实施 |
| 6 | 内核 vsnprintf 深度加固 + fuzzing 纵深 | %s NULL 守卫已加 | 长格式穷举 + 宿主 fuzz 目标 |
| 7 | FD 继承 (P0-6) | Fork 子进程 = 深拷贝空表 (双释放已消除); 真继承待 filedesc 引用计数 | fd_manager_dup + FSOPS.dup 钩子 |
| 8 | xHCI 异步 IN 路径 (P1-32/33 残余) | 同步路径已修复; 异步走 bounce/SG 待实施 | 每页 chained TRB 延伸到异步 |

(End of file - total 71 lines)
