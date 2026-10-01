// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// sched_rip.cpp - Rate-aware 反馈层 + 量子层 (拆分自 sched.cpp)
//   RIP 四层闭环 (信号/基线/控制/统计) + get_dynamic_quantum +
//   eligibility_capped_quantum (EEVDF eligible 预算截断)
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>
#include <atomic/atomic.h>
#include <fs/fc.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <pdef.h>

#include <arch/x86_64/schedule/sched_internal.h>

/* ============================================================
 *  RIP 速率反馈 — 全部定点 Q10 (1.0x = 1024)
 * ============================================================ */
#define RIPRATE_FRAC_BITS  10
#define RIPRATE_INIT       (1ULL << RIPRATE_FRAC_BITS)      /* 1.0x, 文档用 */
#define RIPRATE_ONE        (1ULL << RIPRATE_FRAC_BITS)      /* 1.0x 常量 */
#define RIPRATE_SHIFT      3                                 /* 全局观测 EWMA 1/8 */
#define RIPRATE_MAX_MULT   (10ULL << (RIPRATE_FRAC_BITS - 2))/* 2.5x 上限 (原 4x: 长量子让交互线程"跑过头"带负 lag 入睡, 唤醒要等 avg 追平 — 与 eligible 预算截断配套收敛尾延迟) */
#define RIPRATE_MIN_MULT   (1ULL << (RIPRATE_FRAC_BITS - 2))/* 0.25x 下限 */
#define RIPRATE_OUTLIER_MULT (16ULL << RIPRATE_FRAC_BITS)   /* 离群钳位 16x */

/* 双时间尺度 / 基线 / 死区 / 振荡检测 */
#define RIP_FAST_SHIFT     2    /* 快通道 EWMA 1/4 (捕突发) */
#define RIP_SLOW_SHIFT     4    /* 慢通道 EWMA 1/16 (稳态) */
#define RIP_BASE_SHIFT     6    /* per-thread 基线 EWMA 1/64 */
#define RIP_DEAD_ZONE      (RIPRATE_ONE >> 5)   /* 3.125%: 死区内不动 adj */
#define RIP_OUTLIER_STREAK_MAX 4                /* 连续离群 → 基线重置 */
#define RIP_FAST_W_NORM_Q10  256                /* 快通道常规融合权重 25% */
#define RIP_FAST_W_OSC_Q10   64                 /* 振荡时 6.25% */
#define RIP_OSC_HI  (RIPRATE_ONE >> 2)          /* |dev| 均值 > 25% → 慢模式 */
#define RIP_OSC_LO  (RIPRATE_ONE >> 3)          /* < 12.5% → 恢复 (迟滞) */

/* 短窗口防御. 阈值依据: 典型窗口 = base_quantum(∈[2,15]) × w/1024 ×
 * mult, weight-1024 常态 ~5ms — 门槛必须显著小于典型窗口, 否则会把
 * 正常采样一并关掉; 3ms 下墙钟 ±1ms 量化误差 ≤ 33%. */
#define RIP_MIN_SAMPLE_MS  3

/* 直接修正项的钳位 (± 量子偏移上限) */
#define RIPADJ_MAX        4
#define RIPADJ_MIN       (-4)
/* 老化: 每隔 50ms 无采样收缩一步; 超过 16 个周期直接归零 */
#define RIPRATE_AGING_MS 50ULL
#define RIPRATE_AGING_MAX_STEPS 16ULL
#define RIPRATE_DECAY_SHIFT 4

/* 融合权重 per-CPU: 单写者 = 本核 Switch, 导出读取用 relaxed 原子.
 * 全局单旋钮会让单核噪声振荡把所有核的快通道一起压低. */
rip_fast_weight_ctx rip_fast_weight[MAX_CPU];

/* per-CPU 反馈统计 — 全 u32, 单 cache line 64B (per-CPU 数据无伪
 * 共享, 但热路径少摸一行仍是净赚). samples 以 ~500/s 计 ~100 天
 * 回绕 — 统计量, 回绕良性. */
rip_stats_ctx rip_stats[MAX_CPU];

/* 可选 TSC 校准钩子 — 返回 TSC 每毫秒周期数.
 * 返回 0 (默认弱符号) = 未校准, 采样走墙钟分母.
 * 在别处定义强符号 (必须是同名同签名的普通 C++ 函数, C 文件里
 * 符号对不上) 即可启用 "执行期分母" — 剔除窗口内的中断时间. */
__attribute__((weak)) uint64_t sched_tsc_per_ms(void) { return 0; }

sched_steal_throttle per_cpu_steal_throttle[MAX_CPU];

extern art_tree *pid2proc_tree;
extern art_tree *NOT_RUNQ_P;
extern spinlock_t PID2PROC_TREE_LOCK;
extern spinlock_t PROC_LIST_LOCK;
extern uint64_t sched_pid;
extern uint64_t sched_tid;

sched_padded_u32 per_cpu_steal_cursor[MAX_CPU];


dyn_adjust_ctx dyn_ctx[MAX_CPU];

/* TSC 锁外, cur_tsc 参数传入 */
void dynamic_adjust_quantum(cpu_t *cpu, thread_t *curr_thread,
                                   uint64_t now_ms, uint64_t cur_tsc) {
    uint32_t id = cpu->id;

    if (unlikely(cpu->base_quantum < 2))  cpu->base_quantum = 2;
    if (unlikely(cpu->base_quantum > 15)) cpu->base_quantum = 15;

    dyn_adjust_ctx *dc = &dyn_ctx[id];

    if (likely(dc->last_tsc != 0)) {
        uint64_t elapsed = cur_tsc - dc->last_tsc;
        if (unlikely(curr_thread == cpu->idle_thread)) dc->idle_tsc += elapsed;
        dc->total_tsc += elapsed;
    }
    dc->last_tsc = cur_tsc;

    if (unlikely(now_ms - dc->last_adjust_ms > 100)) {
        if (likely(dc->total_tsc > 0)) {
            uint64_t idle_ratio = (dc->idle_tsc * 100) / dc->total_tsc;
            uint64_t ctx_sw = cpu->sched_stats.context_switches - dc->last_ctx_sw;

            if (idle_ratio > 50) {
                /* 大量空闲: 拉长量子, 摊薄定时器/切换开销 */
                if (likely(cpu->base_quantum < 15)) cpu->base_quantum++;
            } else if (unlikely(idle_ratio < 10 && ctx_sw > 500)) {
                /* 高负载 + 切换风暴: 拉长量子降低切换频率
                 * (缩短量子只会制造更多切换) */
                if (likely(cpu->base_quantum < 15)) cpu->base_quantum++;
            } else {
                if (cpu->base_quantum > 5) cpu->base_quantum--;
                else if (cpu->base_quantum < 5) cpu->base_quantum++;
            }
        }

        dc->idle_tsc = 0;
        dc->total_tsc = 0;
        dc->last_ctx_sw = cpu->sched_stats.context_switches;
        dc->last_adjust_ms = now_ms;
    }
}

/* ============================================================
 *  get_dynamic_quantum — 基准 × 快慢融合倍率 + 有符号修正项
 *
 *    eff = (base × fused_mult >> FRAC) + rip_quantum_adj
 *
 *  fused_mult = (mf·wf + ms·(1024−wf)) >> 10 — 凸组合, 融合值
 *  必落在 [MIN_MULT, MAX_MULT] 内. wf 为本核融合权重 (振荡检测
 *  动态调整: 常态 25% / 振荡 6.25%).
 *  custom_quantum 语义: 既是基准也是天花板 — 倍率只能把它往下
 *  拉, 反馈在 cap 之内双向生效.
 *  mult == 0 (零初始化/从未采样) 按 1.0x 处理.
 * ============================================================ */
uint64_t get_dynamic_quantum(cpu_t *cpu, thread_t *thread) {
    if (unlikely(!thread || thread == cpu->idle_thread)) return cpu->base_quantum;

    uint64_t base;
    if (unlikely(thread->custom_quantum > 0)) {
        base = thread->custom_quantum;
    } else {
        uint64_t weight = likely(thread->weight) ? thread->weight : 1024;
        base = (cpu->base_quantum * weight) / 1024;
    }
    uint64_t mf  = likely(thread->rip_mult_fast) ? thread->rip_mult_fast : RIPRATE_ONE;
    uint64_t msl = likely(thread->rip_mult_slow) ? thread->rip_mult_slow : RIPRATE_ONE;
    uint32_t wf  = __atomic_load_n(&rip_fast_weight[cpu->id].w, __ATOMIC_RELAXED);
    if (unlikely(wf == 0 || wf > (uint32_t)RIPRATE_ONE)) {
        wf = RIP_FAST_W_NORM_Q10;
        rip_stats[cpu->id].wf_clamped++;   /* 数组被写坏的观测 */
    }
    uint64_t mult = (mf * wf + msl * (RIPRATE_ONE - wf)) >> RIPRATE_FRAC_BITS;

    uint64_t q = (base * mult) >> RIPRATE_FRAC_BITS;

    /* 直接修正通道 — 有符号加减 */
    int32_t adj = thread->rip_quantum_adj;
    if (likely(adj > 0)) {
        q += (uint64_t)adj;
    } else if (unlikely(adj < 0)) {
        uint64_t sub = (uint64_t)(-(int64_t)adj);
        q = (q > sub) ? (q - sub) : 1;
    }

    if (unlikely(q < 1)) q = 1;
    /* cap 尊重 custom_quantum 的显式意图 (统一 8×base 截断会把
     * 大值 custom 砍掉 — custom=50/base=2 会变 16) */
    uint64_t cap = cpu->base_quantum * 8;
    if (unlikely((uint64_t)thread->custom_quantum > cap)) cap = thread->custom_quantum;
    if (unlikely(q > cap)) q = cap;
    return q;
}

/* EEVDF 对齐: eligible 预算截断 —— 线程最多跑到 vruntime 追平 avg_vruntime
   (eligible 结束即 slice 结束)。RIP 拉长的量子不再让线程"跑过头"带负 lag
   入睡(唤醒要等 avg 追平, 实测 144 tick 唤醒延迟的根因)。
   模型验证: 公平性极差 0.0056%→0.0000%, 唤醒延迟 144→75 tick(-48%),
   RIP 长量子造成的选取饥饿 70→5 次。 */
uint64_t eligibility_capped_quantum(cpu_t *cpu, thread_t *t) {
    uint64_t q = get_dynamic_quantum(cpu, t);
    uint64_t w = likely(t->weight) ? t->weight : 1024;
    if (likely(cpu->avg_vruntime > t->vruntime)) {
        uint64_t budget = ((cpu->avg_vruntime - t->vruntime) * w) / 1024;
        if (budget < q) q = budget ? budget : 1;
    } else {
        q = 1;   /* vruntime 已 >= avg: eligible 预算耗尽, 给 1ms 快速轮转 */
    }
    return q;
}

/* ============================================================
 *  riprate_update — RIP 速率反馈 (每 tick 对当前线程采样)
 *
 *  信号层: tag = pagemap ^ (cs&3). pagemap 是对齐内核指针, 低 2 位
 *    为 0, 与 RPL 异或无碰撞. tag 变化 = 地址空间或特权级切换,
 *    本窗口 RIP 差分与历史不可比 → 只重臂不采样.
 *    分母优先用 TSC 执行期, 否则墙钟 ms.
 *
 *  基线层: rip_base_self (1/64 慢 EWMA). mult 语义 = "当前节奏相对
 *    自身历史的变化" — 稳态一切归 1.0, 反馈只在阶段变化时起作用
 *    (公平性由 vruntime 承担, 与 mult 正交, 基线漂移只损失整形
 *    精度). 连续 4 次离群 (≥16x) → 基线重播种为当前速率.
 *    cpu->rip_avg_rate 仅作观测更新, 不参与控制.
 *
 *  控制层: 快 1/4 / 慢 1/16 双 EWMA, 各自钳 [0.25x, 4x];
 *    死区 3.125% (防噪声把 adj 顶轨); adj = dev>>6 步进的有符号
 *    偏置, ±4 轨道, 饱和计数暴露. 刻意不做跨通道抗饱和馈入:
 *    mult→quantum→采样→adj→mult 的二阶耦合回路有自激风险,
 *    有界偏置 + 可观测已足够.
 *
 *  统计层: per-CPU 计数 + 64 样本 |dev| 均值 → 振荡检测 (迟滞
 *    25%/12.5%) 调整本核融合权重. 统计先于死区累计 — 死区样本
 *    也是偏差, 收敛判据不能对其失明.
 *
 *  窗口不变量:
 *    progress = rip_now − 上次采样点 RIP (dispatch 时初始化,
 *               每次采样后无条件重臂 — 早退路径不累积窗口)
 *    elapsed  = 实际运行时长 (调用方由 last_run_time 结算, 而非
 *               编程量子 — 被提前打断的窗口不被高估)
 *
 *  已知取舍: RIP 不是完美的 progress 代理 (rep 前缀期间 RIP 不
 *  动, 回跳循环回绕被丢弃) — 可换 IA32_FIXED_CTR0; 墙钟 ms 粒度
 *  噪声由双通道 EWMA 吸收.
 * ============================================================ */
void riprate_update(cpu_t *cpu, thread_t *thread,
                                  uint64_t rip_now, uint64_t cs,
                                  uint64_t wall_ms, uint64_t cur_tsc,
                                  uint64_t now_ms) {
    if (unlikely(!thread || thread == cpu->idle_thread)) return;

    rip_stats_ctx *st = &rip_stats[cpu->id];

    /* 零初始化防御 (双通道) */
    if (unlikely(thread->rip_mult_fast == 0)) thread->rip_mult_fast = RIPRATE_ONE;
    if (unlikely(thread->rip_mult_slow == 0)) thread->rip_mult_slow = RIPRATE_ONE;

    /* ---- 0) 信号标签: 地址空间/特权级可比性 ---- */
    uint64_t tag = (uint64_t)thread->pagemap ^ (uint64_t)(cs & 3);
    if (unlikely(thread->rip_signal_tag == 0 || thread->rip_signal_tag != tag)) {
        thread->rip_signal_tag = tag;
        thread->dispatch_rip = rip_now;        /* 窗口重臂, 不采样 */
        thread->rip_last_tsc = cur_tsc;
        thread->rip_last_sample_ms = now_ms;
        st->tag_invalid++;
        return;
    }

    /* ---- 1) 老化: 按离 CPU 间隔成比例衰减 ----
     * steps = gap / AGING_MS (封顶), 逐周期收缩 1/16; 超过 16 个
     * 周期 (≥800ms 没跑) 直接归位 — 陈旧反馈无意义.
     * 持续运行的线程 gap ≈ 一个 slice → steps = 0, 不衰减.
     * 归零分支不 return — mult/adj 归位后继续采样, 首采样建基
     * 线不受影响. dispatch 处刻意不重置 rip_last_sample_ms:
     * 睡眠时长必须进入老化计算, 否则长睡线程带着陈旧 mult 回来. */
    uint64_t last_sample = thread->rip_last_sample_ms;
    if (unlikely(last_sample != 0)) {
        uint64_t steps = (now_ms - last_sample) / RIPRATE_AGING_MS;
        if (unlikely(steps > RIPRATE_AGING_MAX_STEPS)) {
            thread->rip_mult_fast = RIPRATE_ONE;
            thread->rip_mult_slow = RIPRATE_ONE;
            thread->rip_quantum_adj = 0;
        } else {
            for (uint64_t i = 0; i < steps; i++) {
                /* 收缩不会越过 1.0 (减量 ≤ 差值), 无需再 clamp */
                uint64_t mf = thread->rip_mult_fast;
                if (likely(mf > RIPRATE_ONE))       mf -= (mf - RIPRATE_ONE) >> RIPRATE_DECAY_SHIFT;
                else if (unlikely(mf < RIPRATE_ONE)) mf += (RIPRATE_ONE - mf) >> RIPRATE_DECAY_SHIFT;
                thread->rip_mult_fast = mf;

                uint64_t msl = thread->rip_mult_slow;
                if (likely(msl > RIPRATE_ONE))       msl -= (msl - RIPRATE_ONE) >> RIPRATE_DECAY_SHIFT;
                else if (unlikely(msl < RIPRATE_ONE)) msl += (RIPRATE_ONE - msl) >> RIPRATE_DECAY_SHIFT;
                thread->rip_mult_slow = msl;

                /* (x+15)>>4 = ceil(x/16), 保证 ≥1 步 */
                int32_t adj = thread->rip_quantum_adj;
                if (likely(adj > 0)) {
                    adj -= (int32_t)(((uint32_t)adj + 15) >> RIPRATE_DECAY_SHIFT);
                } else if (unlikely(adj < 0)) {
                    uint32_t mag = (uint32_t)(-(int64_t)adj);
                    adj += (int32_t)((mag + 15) >> RIPRATE_DECAY_SHIFT);
                }
                thread->rip_quantum_adj = adj;
            }
        }
    }
    thread->rip_last_sample_ms = now_ms;

    /* 窗口 TSC 基准: 无条件重臂 — 早退路径不污染下一窗口 */
    uint64_t tsc_dt = cur_tsc - thread->rip_last_tsc;
    thread->rip_last_tsc = cur_tsc;

    if (unlikely(wall_ms == 0)) return;

    /* ---- 2) 执行期分母 ----
     * tri-state 缓存: -1 = 未查询, 0 = 未校准(弱默认), >0 = 周期数.
     * 只缓存"非零值"在默认配置下永远不命中 (0 是合法值).
     * 首次并发查询写同值 — 良性. */
    static int64_t tsc_per_ms_cache = -1;
    int64_t tpm = tsc_per_ms_cache;
    if (unlikely(tpm < 0)) {
        tpm = (int64_t)sched_tsc_per_ms();
        tsc_per_ms_cache = tpm;
    }
    uint64_t elapsed = wall_ms;
    if (likely(tpm > 0)) {
        uint64_t exec_ms = tsc_dt / (uint64_t)tpm;
        if (likely(exec_ms != 0 && exec_ms <= wall_ms)) {
            elapsed = exec_ms;                 /* 纯执行时间, 剔除中断 */
        } else if (unlikely(exec_ms > wall_ms)) {
            st->tsc_mismatch++;                /* 频率估计不可信 → 墙钟兜底 */
        }
        /* exec_ms == 0: 亚毫秒窗口 → 墙钟兜底 */
    }

    /* ---- 3) RIP 差分 (逐点重臂) ---- */
    uint64_t progress = rip_now - thread->dispatch_rip;
    thread->dispatch_rip = rip_now;
    /* 回绕/异常防御: 紧回跳循环的 RIP 倒退 → 无符号回绕成巨值,
     * 该样本丢弃 (窗口已重臂, 不污染下一次) */
    if (unlikely(progress > (1ULL << 44))) { st->outliers++; return; }

    uint64_t obs_rate = progress / elapsed;
    if (unlikely(obs_rate == 0)) {
        /* 停滞样本: 单独计数. dispatch_rip 已在上方重臂, 无需再写.
         * 刻意不更新基线: 从零速率播种基线是错的, 且停滞期基线
         * 冻结正是后续快相位触发 outlier→重置的前提. */
        st->stalled++;
        return;
    }

    /* ---- 4) per-thread 基线 ---- */
    if (unlikely(thread->rip_base_self == 0)) {
        thread->rip_base_self = obs_rate;     /* 首样本只建基线 */
        return;
    }
    uint64_t baseline = thread->rip_base_self;

    /* ---- 4b) 短窗口防御: 只走基线慢通道 ----
     * 关闭两条路径:
     *   a) "mult↓ → 量子↓ → 窗口↓ → 量化噪声↑" 的放大回路;
     *   b) 亚毫秒窗口被记账强制 delta=1 → obs_rate 系统性低估.
     * 基线 1/64 EWMA 天然吸噪, 短窗口仍可跟踪. mult 下降到窗口
     * ≈ 门槛处自然驻留 (慢线程拿 ~3ms 短片 — 正是反馈想要的行
     * 为结果), 恢复走边界波动 + 离 CPU 老化两条路.
     * 碎片窗口也不递增离群 streak — 抢占噪声不驱动假基线重置. */
    if (unlikely(elapsed < RIP_MIN_SAMPLE_MS)) {
        if (obs_rate >= baseline) baseline += (obs_rate - baseline) >> RIP_BASE_SHIFT;
        else                      baseline -= (baseline - obs_rate) >> RIP_BASE_SHIFT;
        thread->rip_base_self = baseline;
        st->short_windows++;
        return;
    }

    /* ---- 5) 观测倍率 + 离群 (对照更新前的基线) ---- */
    uint64_t obs_mult = (obs_rate << RIPRATE_FRAC_BITS) / baseline;
    if (unlikely(obs_mult > RIPRATE_OUTLIER_MULT)) {
        /* 离群钳位而非丢弃: 丢弃会让慢基线永远追不上快线程 */
        obs_mult = RIPRATE_OUTLIER_MULT;
        st->outliers++;
        if (unlikely(++thread->rip_outlier_streak >= RIP_OUTLIER_STREAK_MAX)) {
            /* 连续离群: 基线失真 (异频核迁移/阶段剧变) → 直接重播种.
             * 注意不是"新线程"路径 — base 重设为非零当前速率 */
            thread->rip_base_self = obs_rate;
            thread->rip_mult_fast = RIPRATE_ONE;
            thread->rip_mult_slow = RIPRATE_ONE;
            thread->rip_quantum_adj = 0;
            thread->rip_outlier_streak = 0;
            st->base_resets++;
            return;
        }
        /* 接近触发观测 (streak 2..3 未重置) — 只加计数不改逻辑 */
        if (unlikely(thread->rip_outlier_streak >= (RIP_OUTLIER_STREAK_MAX >> 1))) {
            st->near_reset++;
        }
    } else {
        thread->rip_outlier_streak = 0;
    }

    /* 基线慢速 EWMA (1/64) */
    if (obs_rate >= baseline) baseline += (obs_rate - baseline) >> RIP_BASE_SHIFT;
    else                      baseline -= (baseline - obs_rate) >> RIP_BASE_SHIFT;
    thread->rip_base_self = baseline;

    /* 全局基线: 纯观测, 不参与控制 (单写者 = 本核 Switch, 安全) */
    {
        uint64_t *g = &cpu->rip_avg_rate;
        if (unlikely(*g == 0)) *g = obs_rate;
        else if (obs_rate >= *g) *g += (obs_rate - *g) >> RIPRATE_SHIFT;
        else                     *g -= (*g - obs_rate) >> RIPRATE_SHIFT;
    }

    /* ---- 6) 双时间尺度 EWMA ----
     * 先比大小再加减: 无符号 (obs − avg) 在 obs < avg 时回绕成
     * 2^64−d, 右移后 ≈ 2^61 而非 d/8 — 一次"应下调"的采样会把
     * 值炸飞. */
    uint64_t mf = thread->rip_mult_fast;
    if (obs_mult >= mf) mf += (obs_mult - mf) >> RIP_FAST_SHIFT;
    else                mf -= (mf - obs_mult) >> RIP_FAST_SHIFT;

    uint64_t msl = thread->rip_mult_slow;
    if (obs_mult >= msl) msl += (obs_mult - msl) >> RIP_SLOW_SHIFT;
    else                 msl -= (msl - obs_mult) >> RIP_SLOW_SHIFT;

    if (unlikely(mf > RIPRATE_MAX_MULT))  mf = RIPRATE_MAX_MULT;
    if (unlikely(mf < RIPRATE_MIN_MULT))  mf = RIPRATE_MIN_MULT;
    if (unlikely(msl > RIPRATE_MAX_MULT)) msl = RIPRATE_MAX_MULT;
    if (unlikely(msl < RIPRATE_MIN_MULT)) msl = RIPRATE_MIN_MULT;
    thread->rip_mult_fast = mf;
    thread->rip_mult_slow = msl;

    /* ---- 7) 统计层 (先于死区: 噪声也是偏差) ---- */
    int64_t dev = (int64_t)obs_mult - (int64_t)RIPRATE_ONE;
    st->samples++;
    st->abs_dev_sum += (uint32_t)(dev < 0 ? -dev : dev);
    if (unlikely(++st->dev_window >= 64)) {
        st->mean_abs_dev = st->abs_dev_sum >> 6;
        st->abs_dev_sum = 0;
        st->dev_window = 0;
        /* 振荡检测 (迟滞): >25% 降快权重 / <12.5% 恢复 */
        uint32_t cur_w = __atomic_load_n(&rip_fast_weight[cpu->id].w, __ATOMIC_RELAXED);
        if (unlikely(cur_w == 0)) cur_w = RIP_FAST_W_NORM_Q10;
        uint32_t w = cur_w;
        if (unlikely(st->mean_abs_dev > RIP_OSC_HI))     w = RIP_FAST_W_OSC_Q10;
        else if (likely(st->mean_abs_dev < RIP_OSC_LO))  w = RIP_FAST_W_NORM_Q10;
        if (unlikely(w != cur_w)) {
            __atomic_store_n(&rip_fast_weight[cpu->id].w, w, __ATOMIC_RELAXED);
        }
    }

    /* ---- 8) 死区: |dev| < 3.125% 不动 adj ---- */
    if (likely(dev > -(int64_t)RIP_DEAD_ZONE && dev < (int64_t)RIP_DEAD_ZONE)) {
        st->dead_zone++;
        return;
    }

    /* ---- 9) 修正通道: 比例步长 + 轨道钳位 (饱和计数暴露) ---- */
    int32_t step = (int32_t)(dev >> 6);
    if (unlikely(step == 0)) step = (dev > 0) ? 1 : -1;
    int32_t adj = thread->rip_quantum_adj + step;
    if (unlikely(adj > RIPADJ_MAX)) { adj = RIPADJ_MAX; st->sat_hi++; }
    if (unlikely(adj < RIPADJ_MIN)) { adj = RIPADJ_MIN; st->sat_lo++; }
    thread->rip_quantum_adj = adj;
}

