# Goal status and verification ledger

> The goal has three parts: (A) e1000 driver + (B) lwIP porting completion + (C) DPDK-style high-performance rework,
> plus the production-grade checklist (SMP concurrency / long-term stability / fault injection / security / full semantics / performance profiling /
> regression gating / observability). Checked item by item, annotated truthfully.

## 🏁 Milestone: four-source audit fix list —— ✅ all cleared

All six tiers cleared: **P0 ×18** (including two convergence items that were reverted and redone: the TLB ACK fence —
root cause of the 400M pause spins under TCG → capped at 100ms wall clock, measured 0 timeouts; and the AHCI
per-request DMA buffer that replaced the lock spanning the sti window), **P1 ×36**, **P2 ×8** (cross-core ABA
isolation stack foreign_head, AHCI release barrier, QSBR 64 slots), **P3 ×21**, **P4 ×8** (allocator host chaos
test — which also caught a real MCB allocation-granularity bug; fc writeback content verification), **P5 ×19**.
Verification after each change set: BUILD_EXIT=0 + GOLDEN PASS + QEMU ping 3 + 0 exceptions.

## 🏁 Roadmap convergence

| # | Roadmap item | Status |
|---|---|---|
| #7 | FD inheritance (filedesc snapshot-style deep copy) | ✅ also confirmed two latent defects (rb_postorder_iter callback contract + exec FDMan release not nulling) |
| #2 | proc lifecycle TOCTOU | ✅ ProcessAddThread re-check inside lock + reaper FDMan reclamation into lock (5 call sites closed) |
| #6 | vsnprintf fuzz | ✅ ASAN host fuzz + real out-of-bounds read fix (NUL-terminated format string) |
| — | fsync production activation | ✅ fclose + implicit dirty-page flush at process exit |
| — | Sleep/timer wheel revived | ✅ PIT::Sleep dual mode (scheduler ready → real sleep) |
| — | RX burst observability | ✅ rx_burst_avg (DPDK metric) |
| — | Static analysis | ✅ clang --analyze 9 files + 2 defensive fixes (fc LRU double-advance + rbtree sibling guard) |

## A. e1000 driver —— ✅ done

- 82574L (QEMU e1000e) complete driver: probe/reset/EEPROM MAC/dual ring 256/IRQ+polling dual mode
- Bug-fix history: RDT ownership, _memcpy transposed order, GetPhysics in-page offset,
  pbuf double free, PIT::Sleep hang during probe, early PIT busy-wait guard
- Multi-core TX spinlock serialization + ring-full backoff

## B. lwIP porting —— ✅ done (production default: polling + copy path)

- End-to-end verification: DHCP lease (10.0.2.15), ICMP ping consecutive replies, multi-core TX flood 2.2-2.7k frames/s
- Porting fixes: lwipopts (software checksum/alignment/pool), cc.h DIAG varargs chain fix,
  sys_now monotonic milliseconds, ACD disabled (slirp ARP proxy), ip4addr_ntoa static-buffer pitfall
- Network thread stack 32KB (16KB stack overflow = one of the deep root causes of all intermittent crashes)

## C. DPDK-lite —— 🟡 base complete, two experimental items documented

| DPDK concept | Status |
|---|---|
| PMD polling | ✅ production path |
| Multi-core TX concurrency | ✅ spinlock + 3-core flood verification |
| Per-core private / lock-free | 🟡 single RX queue (dual queue = MSI-X blocking item) |
| Zero-copy RX | 🟡 experimental (NET_ZEROCOPY=0): index back-derivation + double free fixed, still silent after first stat (suspects narrowed to 2 items, see dpdk-lite.md) |
| Checksum offload / MSI-X | ❌ blocked on register-manual verification |
| Stats/observability | ✅ counters + RTT P50/P90/P99 + stall self-diagnosis |

## Production-grade checklist item by item

| Pillar | Status | Evidence |
|---|---|---|
| SMP concurrency correctness | ✅ existing (SLUB/FC/sched multithreaded suites) + network multi-core stress + **110-item P0/P2 concurrency fixes all cleared** | tests/ + soak |
| Long-term stability | ✅(evidence) closing soak chain: 10/15/30min + 40min multi-segment all 0 assertions 0 exceptions 0 stalls; final 30min: tx=1.29M frames (82.7MB) sustained at 2429/s, ping=185 consecutive, loss=0; **re-tested after every milestone** | serial.log |
| Fault injection | ✅ OOM/page allocation/disk errors (newly tested)/RX OOM injection all closed-loop | fc_reg + net injection |
| Security audit | 🟡 syscall entry audit ✅, SECURITY.md ✅, trust-bit model documented; fuzzing/defense-in-depth ❌ | SECURITY.md |
| Full semantics | ✅ writeback/dirty-page/fsync/OOM semantics audit (fc-semantics.md); permissions = trust bits (as-is) | fc-semantics.md |
| Performance profiling | ✅ RTT percentiles + sched_bench real-kernel report (step/osc pass; pollute=real finding, shortwin=semantic mismatch) | scheduler.md |
| Regression gating | ✅ three-tier gating: host CI + golden diff + QEMU network smoke | tests/ |
| Observability | ✅ counters/rates/RTT/panic to serial/stall self-diagnosis | per-change verification |

## Remaining technical debt (by priority)

| # | Debt | Status | Fix design |
|---|---|---|---|
| 1 | pollute phase RIP multiplier residual variance | 3x convergence achieved (accumulator), full pass awaiting in-window instrumentation | numerator-side / phase-transition instrumentation |
| 2 | exit reclamation path spinlock UAF | lock pairing converged (TOCTOU window closed); full cure still leaves proc refcounting | proc refcounting (cure) |
| 3 | Zero-copy remaining suspects | see dpdk-lite.md (suspects narrowed to 2 items) | pbuf lifetime runtime tracking |
| 4 | sched_bench shortwin sampling | switch-point sampling already online (stale docs fixed) | ✅ converged |
| 5 | Checksum offload / MSI-X multi-queue | blocked on 82574 datasheet | implement after register verification |
| 6 | Kernel vsnprintf deep hardening + fuzzing in depth | fuzz + out-of-bounds read fix ✅ | ✅ converged |
| 7 | FD inheritance | fd_manager_dup ✅ (lock/OOM rollback included) | ✅ converged |
| 8 | xHCI async IN path | sync path fixed; async via bounce/SG to be implemented (blocked on real-hardware verification) | per-page chained TRB extended to async |
| 9 | Audit latest batch | High-risk 14 items: 11 fixed + 3 false positives; medium-risk 13 items: 6 fixed + 5 not applicable/already covered + allocator integration analysed (needs boot-order coordination) + delayed detection is by design (SLAB detects at refill, SLUB immediate); design debt analysed (semantics correct, the rename that caused a boot hang in testing was reverted, migration plan on record); hygiene 16 items: 15 closed (incl. 3 outdated claims: fb.cpp copy init / x86mem MIT added / libc line numbers pointed at wrong file) + 1 roadmap item (-Wno batched removal) | see the audit records |

## 🏁 kmod / 整个内核组件化（本轮目标）—— ✅ 实测完成

在原有的宏内核之上引入 Tier0 私有契约型模块链接与治理机制，并把整个内核的组件
迁移为同一张组件图（Tier 0 固定机制 + Tier 1 内建组件 + Tier 2 可加载 .ko）。

- **Tier0 机制**：ET_REL 加载器（.in/.out/.kmod_manifest、受限重定位集、严格边界校验）、
  Kahn 拓扑图/环检测、能力策略（STRICT/OPEN/DISABLED）、事务化加载与逆序回滚、
  热替换（quiesce/epoch 排空/看门狗/原子切换/失败回退）、卸载引用计数硬门禁、
  固定 syscall 边界（0..63 固定契约 + 128..511 动态命名空间 + 哨兵治理表）、
  座位（seat）机制、内建组件模型（.kbuiltin.* + KmodBootComponents 启动拓扑）。
- **组件化范围**：37 个内建组件（平台 ACPI/APIC/IRQ/时间/SMP、调度核心/安装/策略、
  分配策略、设备/PCI 核心/枚举、AHCI/NVMe/xHCI/e1000/PS2、VFS/ext4/FatFs/rootfs、
  lwIP/netdev、framebuffer、user.init、7 个 syscall 家族）+ 14 个 .ko 演示模块
  （依赖/私有契约回填、事务回滚、能力/环负向、随机/调度/分配策略座位热替换、
  动态 syscall 槽与固定槽覆写热替换）。
- **启动序列**：x86_64_init 只保留 Tier0（Serial/GDT/IDT/FPU/PMM/VMM/SLAB/SLUB/
  SMEP/SIMD/syscall 入口/KmodInit），其余由 KmodBootComponents() 按 manifest 依赖
  拓扑初始化；Tier0 收尾 sti+IPI+idle。
- **验证证据**：宿主门禁 6/6 PASS（含 ASAN/UBSAN、2000 次 fuzz）+
  内核构建零错误 + QEMU 无头串口 KMOD_SELFTEST PASS 11/11（37 builtin ready、
  boot load 2/2、desktop/DOOM/lwIP 正常、0 异常/0 stall）。
- **文档**：docs/kernel-modules.md（机制）、docs/kernel-components.md（组件台账/
  Tier 划分/迁移状态/已知限制与路线图）。
- **迁移中修复的 Tier0 问题**：IRQ0 解除屏蔽早于 PIT handler 注册（新增 kernel.arch.irq
  组件 + 引导期 cli）；引导栈深度（start.S 显式 64KiB __boot_stack）；partition 头文件
  引用参数声明 latent bug；selftest 移到 sti 前单线程以确定性暴露模块治理错误。

## 🏁 Tier0 共享服务层：klib 等公共工具共享给每个模块（当前目标）—— ✅ 实测完成

在「src → modules 全模块化」基础上，把 Tier0 公共工具开放给**所有模块**（37 个内建
组件 + 全部 .ko），并补齐 provider="kernel" 导入的契约校验缺口。

- **两种共享方式**：
  1) 直接符号：klib C 链接函数加入内核 .kout（57 项导出）：str、kprintf 格式化族、
     hashmap_*、art_*、spinlock_*、kcalloc/krealloc/kmalloc_aligned；.ko 直接调用
     即由加载器按 ELF 未定义符号名解析，无需 .in。
  2) 契约 ops：kernel.klib.v1（字符串/格式化/qsort/bitmap/kld/随机/panic）、
     kernel.sync.v1（自旋锁/中断状态）、kernel.algo.v1（hashmap/ART/Queue/FIFO/List）；
     模块用 KMOD_IMPORT_KLIB/SYNC/ALGO，治理层校验 contract 相等 + abi 区间 +
     expect_size == sizeof(ops)，失败整批事务回滚。
- **治理补齐**：provider="kernel" 导入从"仅名字匹配"升级为与模块间导入同等的
  契约/版本/尺寸强校验（新增 ind_kernel_export_in）。
- **共享服务导出源单一**：kernel_services.cpp 同时供内核 .kout 与宿主测试表，
  防止两套表漂移；宿主用 std libc 薄包装验证同一 initializer。
- **端到端示例**：endor.demo.klib .ko 同时使用直接符号 + klib/sync ops，init 自检
  通过后导出 magic；selftest shared-klib-contract 加载→回读 magic→卸载（12/12）。
- **修复**：扩大导出面激活了 str.c 中 toi 的 latent isdigit 缺失 include
  （此前被 --gc-sections 回收未暴露），已在 str.c 补 klib/ctype.h。
- **验证**：内核 clean build 零错误；modules-check 15/15；宿主 6/6 + ASAN 6/6；
  QEMU KMOD SMOKE: PASS、KMOD_SELFTEST PASS 12/12、kernel exports: 57、
  37 builtin ready、0 STALL；文档 kernel-modules.md §4.1 / kernel-components.md §6/§10
  已更新（含排除项与 ABI 规则）。
