# SMP：多核启动与 per-CPU

源码：`kernel/src/arch/x86_64/smp/smp.cpp`（约 9 KB）。

## 1. 启动流程

通过 **Limine multiprocessor 协议**枚举 CPU，BSP 引导 AP：

```text
smp_init()  (BSP 上)
  ├─ 记录 bsp_cpu->lapic_id，建立 apic_id_to_logical[] 映射
  ├─ smp_setup_thread_queue(bsp)   // 每 CPU 一棵 RBTree runqueue
  ├─ smp_setup_kstack(bsp)         // 内核栈 + TSS IST/RSP0
  ├─ EnableFSGSBASE(bsp)
  ├─ simd_cpu_init(bsp)
  └─ 遍历 mp_response->cpus[]：
        给每个 AP 分配 cpu_t，填 lapic_id/logical_id，
        设 mp_info->goto_address = smp_cpu_init，
        BSP 在 pause 循环里等 started_count == 其余核数
```

每个 AP 落到 `smp_cpu_init(mp_info)`：

```text
切到 kernel_pagemap
  → GDT::Init(logical_id) + idt_reinit(logical_id)
  → 拿 smp_lock，填 cpu_t：self/id/lapic_id，初始化 cslab 空闲链表
  → wrmsr KERNEL_GS_BASE / IA32_GS_MSR = cpu_t*   ← this_cpu() 的根基
  → LAPIC::Init + InitTimer
  → EnableFSGSBASE(cpu)（不支持则 WRFSBASE 回退到普通版）
  → smp_setup_kstack
  → idt_install_irq_cpu(SCHED_VEC, Schedule::Internal::Switch)
  → syscall_init()
  → smp_setup_thread_queue()
  → sse_enable / fpu_init / simd_cpu_init / cpu_simd_mask
  → enable_smep_smap()
  → 清零 tv1/tv2/tv3 定时器桶
  → 分配 per-CPU file_cache 并注册写回回调
  → started_count++，更新 smp_last_cpu
  → LAPIC::Oneshot(SCHED_VEC, base_quantum*ticks)   // 关键：开第一个 tick
  → sti
  → sched_idle()   ← 不返回；第一次 Switch 把这里的上下文快照成 idle 线程
```

## 2. per-CPU 指针怎么拿

- `this_cpu()` 在 `smp_started` 后直接 `movq %%gs:0, %0`——GS 基址在 AP 启动时写入 `IA32_GS_MSR`，每个核读到自己的 `cpu_t*`，无锁、无数组索引。
- `smp_started` 之前（极早期）一律返回 `bsp_cpu`。
- per-CPU 数据：`dyn_ctx[]`（调度反馈统计）、`per_cpu_steal_throttle[]`、`per_cpu_steal_cursor[]`、`g_tlb_batch[]`、`cslab`（SLAB 空闲链）、`file_cache`。

## 3. 硬件能力探测与降级

| 特性 | 探测 | 不支持时 |
|---|---|---|
| FSGSBASE | `cpuid.(7,0).ebx & (1<<5)` | `WRFSBASE` 回退到普通版本，不崩 |
| SMEP/SMAP | `enable_smep_smap()` | — |
| XSave / AVX | `simd_cpu_init`，存 `XsaveSize/MaskLo/MaskHi` | 线程 fx_area 按实际尺寸分配 |
| SSE/FPU | `fpu_init()` | 不支持直接 `hcf()` |

## 4. 两个关键的启动期死锁/卡顿修复

源码注释里记录：

1. **AP 必须在 idle 前 arm 第一个 periodic tick**。如果 AP 在 LAPIC 定时器关闭状态下就 `hlt`，而远端唤醒 IPI 又恰好丢失，它会一直睡到下一次无关 IPI——实测约 14 秒卡顿。先 arm oneshot 再 hlt，保证每个 base slice 至少被调度器拉回来一次。
2. **IPI 唤醒**：push 任务到一个正 hlt 的核后，立刻 `LAPIC::IPI(target->lapic_id, SCHED_VEC)`，不等它自己醒来。

## 5. 中断与定时器桶

- 每个 CPU 有 `tv1/tv2/tv3` 三级时间轮（典型内核定时器分级），`timer_cpu` 字段记录线程定时器归哪个核管；线程被 steal/push 时会同步改 `timer_cpu`，避免定时器在旧核到期。
- SCHED_VEC 是调度器 IPI 向量；`LAPIC::IPI` / `IPIOthers` 用于跨核调度。
- IOAPIC 负责把 GSI 路由到指定 LAPIC；PIT GSI0 在 BSP 上被显式重定向到向量 32（见 architecture.md）。

## 6. 亲和性

- `cpu_simd_mask(cpu)` 给出该核的 SMT/拓扑掩码；
- 负载均衡（见 scheduler.md）优先在同掩码内 push/steal，避免跨 L3 搬线程；
- GUI 合成器的 worker 也按在线 CPU 数 pin，一核一条水平条带（见 gui.md）。
