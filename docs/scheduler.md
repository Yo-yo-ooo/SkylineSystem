# 调度器 —— vruntime 调度 + RIP 速率反馈（曾用名 3EVDF）

源码：`kernel/src/arch/x86_64/schedule/sched.cpp`、`task.cpp`、`timer.cpp`、`syscall/`。

> 本文档只描述代码**实际做到**的事，并明确列出**没有做到**的事。

## 1. 实际结构

每个 CPU 一棵**红黑树**运行队列，键为 `vruntime`，节点增广 `min_vruntime_subtree`（子树最小值），比较点带 `PREFETCH_R`。线程按权重（`sched_prio_to_weight[16]`）得到基准时间片，`vruntime` 按**真实流逝毫秒 × 1024 / weight** 推进，`avg_vruntime` 按负载加权速率推进。

`Pick()` 只用 `vruntime` 与 `avg_vruntime` 做选择：沿"子树含 eligible 节点"的分支下降，取最小者。

## 2. RIP-progress-rate 反馈（3EVDF 部分）—— 它改什么、不改什么

每个 tick 对当前线程采样 RIP 推进量，经 Q10 定点双通道 EWMA 得到快/慢倍率 `mult`（钳制 [0.25x, 4x]），加一个 ±4 tick 的有符号修正项 `adj`，叠加出**每个线程的时间片长度**：

```text
eff_quantum = (base × fused_mult >> 10) + adj
```

该结果只用于 **LAPIC oneshot 定时器的时长**（`sched.cpp` 中 `get_dynamic_quantum()` 的调用点）。

**它不改的事（重要）**：

- `Pick()` 中**不出现** `mult` / `adj` —— 选择仍由 `vruntime` 决定；
- `vruntime` 按真实时间记账，**不被倍率缩放**；
- 因此反馈影响的是**抢占节奏/中断延迟**（忙等线程更频繁被打断、推进快的线程获得更长的不间断运行），**不改变 CPU 份额**；
- 公平性由 vruntime 机制承担，与速率反馈正交。

## 3. EEVDF 成分的真实程度

- `calibrate_and_set_deadline()` 在**每次入队**时把 `vruntime` 钳进 `[avg−q, avg+2q]` 并令 `deadline = vruntime + base_quantum`；
- 时间片**未按权重缩放**（源码注释自认"有意简化"），于是 deadline ≡ vruntime + 常数，eligibility 判定与虚拟 deadline 实际退化为冗余控制 —— 队列序即 vruntime 序；
- 所以这是**EEVDF 结构的子集**，不是完整的 EEVDF。README 与本文档均按此描述。

## 4. 动态基准量子与老化

`dynamic_adjust_quantum()` 每 100ms 按空闲占比/上下文切换数调整 `base_quantum`（钳制 [2,15] tick）。速率反馈侧：

- 50ms 未采样 → `mult` 按 1/16 步长向 1.0 收缩、`adj` 向 0 收缩（源码注释记录过 `(x+7)>>4` 在 |adj|≤8 时恒 0 的老 bug）；
- 离群观测（>16x）钳位到 16x；
- 新线程 `mult==0` 按 1.0 处理。

## 5. SMP 负载均衡

- **Active push（`TryPush`）**：本核盈余且线程 ≥2 时，按总权重找最轻目标，优先**同 SIMD 特性掩码**（注意：`cpu_simd_mask()` 是 SIMD 指令集位图，**不是 SMT 拓扑**；内核里没有 APIC-ID/核兄弟数据），双锁按 `cpu.id` 排序防 ABBA，批量 `SCHED_STEAL_BATCH=8`，推完若目标 hlt 立即发 IPI 唤醒。
- **Lazy steal（`StealThread`）**：每 8 次节流扫一次；先同掩码后任意核；`spin_trylock` 上限 100 次；偷到先标 `THREAD_TRANSFER` 再换锁入队。

## 6. 抢占与上下文切换

`Schedule::Switch()`（SCHED_VEC ISR）：先停 LAPIC 定时器 → `this_cpu==nullptr` 也 EOI → AP 早期窗口重 arm 返回 → `preempt_count > 1` 不清标志只重排 oneshot → 自愿 `yield()` 走独立 `yield_request_flags`。

**已知缺陷（未修）**：`CheckPreempt()` 在代码树中没有调用者，`preempt_count` 归零后的重新触发依赖下一次定时器 tick。

## 7. 已知修复与新线程记账

新线程的 `last_run_time` 曾在所有创建点未初始化（首片 `delta = uptime`，抬飞 vruntime）；现已**在创建点初始化**并在记账处对 `==0` 做防御。线程创建点均 `memset` 后赋值，行为确定。

## 8. 测试现状（诚实）

**没有调度质量基准。** `sched_bench.cpp` 只校验 EWMA 数值的收敛行为（过冲/稳态步进），不测公平性、唤醒延迟或尾延迟。任何"实测流畅"的说法均未写入本文档 —— 桌面流畅性目前由**独立光标层 + 合成器结构**保证，而非由调度器反馈的份额控制。

## 9. 调参常量速查

| 常量 | 值 | 含义 |
|---|---|---|
| `SCHED_STEAL_BATCH` | 8 | 单次 push/steal 批量 |
| `SCHED_STEAL_THROTTLE` | 8 | steal 扫描节流 |
| `ZOMBIE_RECLAIM_THRESHOLD/BATCH` | 8 / 16 | 僵尸线程回收触发点与批量 |
| `RIPRATE_AGING_MS` | 50 | 速率老化窗口 |
| `RIPADJ_MIN/MAX` | -4 / +4 | 直接修正项范围 |
| `base_quantum` 初始 | 5 tick | 约 5 ms |
