// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// sched.cpp - Rate-aware EEVDF (REEVDF) Schedule ALGO IMPL
//
// 结构总览:
//   公平层  每CPU运行队列 (deadline 排序 rb-tree + 子树最小 vruntime
//           增广); vruntime/虚拟时钟/资格判定; lag 钳位
//   粒度层  quantum = base × weight/1024 × fused_mult + adj
//   反馈层  四层闭环: 信号 (pagemap^ring 标签, TSC 执行期分母) /
//           基线 (per-thread 慢 EWMA) / 控制 (快慢双通道 + 死区 +
//           有符号修正) / 统计 (per-CPU 计数 + 振荡检测)
//   SMP     push (过剩主动推) + pull (空闲偷取) 双向均衡
//   回收    僵尸批量搬出锁外释放
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

#define SCHED_STEAL_BATCH 8
#define ZOMBIE_RECLAIM_THRESHOLD 8
#define ZOMBIE_RECLAIM_BATCH 16
#define SCHED_STEAL_THROTTLE 8
/* 推送门槛: 仅当 my_weight > target × (1 + 1/2^SHIFT) 才推.
 * shift 越小门槛越高 (越保守). */
#define SCHED_PUSH_GAP_SHIFT 2


/* ============================================================
 *  RIP 速率反馈 — 全部定点 Q10 (1.0x = 1024)
 * ============================================================ */
#define RIPRATE_FRAC_BITS  10
#define RIPRATE_INIT       (1ULL << RIPRATE_FRAC_BITS)      /* 1.0x, 文档用 */
#define RIPRATE_ONE        (1ULL << RIPRATE_FRAC_BITS)      /* 1.0x 常量 */
#define RIPRATE_SHIFT      3                                 /* 全局观测 EWMA 1/8 */
#define RIPRATE_MAX_MULT   (4ULL  << RIPRATE_FRAC_BITS)     /* 4x 上限 */
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
struct alignas(64) rip_fast_weight_ctx { volatile uint32_t w; char pad[60]; };
static rip_fast_weight_ctx rip_fast_weight[MAX_CPU];

/* per-CPU 反馈统计 — 全 u32, 单 cache line 64B (per-CPU 数据无伪
 * 共享, 但热路径少摸一行仍是净赚). samples 以 ~500/s 计 ~100 天
 * 回绕 — 统计量, 回绕良性. */
struct alignas(64) rip_stats_ctx {
    uint32_t samples;        /* 有效样本数 */
    uint32_t outliers;       /* 回绕 / 离群钳位 */
    uint32_t tag_invalid;    /* pagemap/ring 切换失效 */
    uint32_t short_windows;  /* 短窗口防御命中 */
    uint32_t stalled;        /* obs_rate == 0 的停滞样本 */
    uint32_t dead_zone;      /* 死区命中 */
    uint32_t sat_hi;         /* adj 顶轨 */
    uint32_t sat_lo;         /* adj 底轨 */
    uint32_t base_resets;    /* 基线重置 (连续离群) */
    uint32_t tsc_mismatch;   /* TSC/墙钟矛盾 → 回退墙钟 */
    uint32_t mean_abs_dev;   /* 最近 64 样本 |dev| 均值 (Q10) */
    uint32_t abs_dev_sum;    /* 窗口累计 (≤ 64×15360, u32 安全) */
    uint32_t dev_window;
    uint32_t near_reset;     /* streak ≥ 半程未触发 (接近基线重置) */
    uint32_t wf_clamped;     /* 融合权重防御分支命中 (数组写坏) */
    char     pad[4];
};
static rip_stats_ctx rip_stats[MAX_CPU];

/* 可选 TSC 校准钩子 — 返回 TSC 每毫秒周期数.
 * 返回 0 (默认弱符号) = 未校准, 采样走墙钟分母.
 * 在别处定义强符号 (必须是同名同签名的普通 C++ 函数, C 文件里
 * 符号对不上) 即可启用 "执行期分母" — 剔除窗口内的中断时间. */
__attribute__((weak)) uint64_t sched_tsc_per_ms(void) { return 0; }

struct alignas(64) sched_steal_throttle { uint32_t skip; char pad[60]; };
static sched_steal_throttle per_cpu_steal_throttle[MAX_CPU];

extern art_tree *pid2proc_tree;
extern art_tree *NOT_RUNQ_P;
extern spinlock_t PID2PROC_TREE_LOCK;
extern spinlock_t PROC_LIST_LOCK;
extern uint64_t sched_pid;
extern uint64_t sched_tid;

struct alignas(64) sched_padded_u32 { uint32_t v; };
static sched_padded_u32 per_cpu_steal_cursor[MAX_CPU];

uint32_t sched_prio_to_weight[16] = {
    /* 0 */ 8192, /* 1 */ 6553, /* 2 */ 5242, /* 3 */ 4194,
    /* 4 */ 3355, /* 5 */ 2684, /* 6 */ 2147, /* 7 */ 1717,
    /* 8 */ 1374, /* 9 */ 1099, /* 10 */ 879, /* 11 */ 703,
    /* 12 */ 562, /* 13 */ 450, /* 14 */ 360, /* 15 */ 288
};

static_assert(sizeof(sched_prio_to_weight)/sizeof(sched_prio_to_weight[0]) == 16,
              "sched_prio_to_weight must have 16 entries");

volatile bool need_resched_flags[MAX_CPU] = {false};
/* Set by an explicit Schedule::Yield(): unlike a timer tick, a voluntary
 * yield MUST hand the CPU to another runnable thread even when the runqueue
 * holds only that single contender (otherwise the lockless fast path keeps
 * running the caller and the lone peer is never scheduled). */
volatile bool yield_request_flags[MAX_CPU] = {false};


static inline uint64_t sced_rdtsc() {
    uint32_t lo, hi;
    asm volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

struct alignas(64) dyn_adjust_ctx {
    uint64_t last_tsc;
    uint64_t last_ctx_sw;
    uint64_t idle_tsc;
    uint64_t total_tsc;
    uint64_t last_adjust_ms;
};
static dyn_adjust_ctx dyn_ctx[MAX_CPU];

/* TSC 锁外, cur_tsc 参数传入 */
static void dynamic_adjust_quantum(cpu_t *cpu, thread_t *curr_thread,
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
static inline uint64_t get_dynamic_quantum(cpu_t *cpu, thread_t *thread) {
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
static inline void riprate_update(cpu_t *cpu, thread_t *thread,
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

static void reclaim_zombie_list(cpu_t *cpu, thread_t *head) {
    thread_t *z = head;
    while (likely(z)) {
        thread_t *next = z->zombie_next;
        if (likely(next)) PREFETCH_R(next);
        Schedule::FreeThreadResources(z);
        kfree(z);
        cpu->sched_stats.zombie_reclaims++;
        z = next;
    }
}

extern void sys_sysinfo_idle_refresh(void);
void sched_idle() {
    while (true) {
        cpu_t *cpu = this_cpu();
        PREFETCH_RH(&cpu->zombie_list);
        thread_t *zombie_head = nullptr;

        uint64_t rflags = spin_lock_irqsave(&cpu->sched_lock);
        if (unlikely(cpu->zombie_list)) {
            zombie_head = cpu->zombie_list;
            cpu->zombie_list = nullptr;
            cpu->zombie_count = 0;
        }
        spin_unlock_irqrestore(&cpu->sched_lock, rflags);

        reclaim_zombie_list(cpu, zombie_head);

        Schedule::DrainProcZombieList(cpu);

        file_cache_idle_handler(cpu->file_cache);
        sys_sysinfo_idle_refresh();
        /* Self-remedy: never rely solely on a remote wakeup IPI, which can be
           missed while a CPU halts with interrupts briefly closed or before its
           first scheduler tick is armed. If a runnable thread is already queued
           on this CPU, enter the scheduler directly instead of blindly halting.
           The periodic LAPIC SCHED tick bounds worst-case latency to one slice. */
        if (unlikely(cpu->thread_count > 0)) {
            asm volatile("int %0" :: "i"(SCHED_VEC));
        }
        asm volatile("sti; hlt; cli" ::: "memory");
    }
}

static inline thread_t* safe_get_current_thread(cpu_t *cpu, bool &invalid) {
    thread_t *t = cpu->current_thread;
    if (unlikely((uintptr_t)t < 0xFFFF800000000000)) {
        /* Corrupted/early current: recover to idle silently. A serial print on
           this per-tick hot path is itself a major source of mouse stutter. */
        invalid = true;
        return cpu->idle_thread;
    }
    invalid = false;
    return t;
}

cpu_t *get_lw_cpu(cpu_t *ref_cpu) {
    cpu_t *lw_cpu = nullptr;
    uint32_t ref_mask = ref_cpu ? cpu_simd_mask(ref_cpu) : 0;
    const int32_t last = smp_last_cpu;
    for (int32_t i = 0; i <= last; i++) {
        cpu_t *cpu = smp_cpu_list[i];
        if (likely(i < last)) PREFETCH_R(smp_cpu_list[i + 1]);
        if (unlikely(cpu == nullptr)) continue;
        if (ref_cpu && cpu_simd_mask(cpu) != ref_mask) continue;
        if (unlikely(!lw_cpu)) { lw_cpu = cpu; continue; }
        uint64_t current_weight = cpu->total_weight + (cpu->current_thread ? cpu->current_thread->weight : 0);
        uint64_t lowest_weight  = lw_cpu->total_weight + (lw_cpu->current_thread ? lw_cpu->current_thread->weight : 0);
        if (current_weight < lowest_weight) lw_cpu = cpu;
    }
    return likely(lw_cpu) ? lw_cpu : (ref_cpu ? ref_cpu : this_cpu());
}

static int thread_rb_cmp(const rb_node_t *a, const rb_node_t *b) {
    const thread_t *ta = container_of(a, thread_t, rb_node);
    const thread_t *tb = container_of(b, thread_t, rb_node);
    if (ta->deadline < tb->deadline) return -1;
    if (ta->deadline > tb->deadline) return 1;
    if (ta->id < tb->id) return -1;
    if (ta->id > tb->id) return 1;
    return 0;
}

static inline thread_t* rb_to_thread(rb_node_t* node) {
    return container_of(node, thread_t, rb_node);
}

static inline void calibrate_and_set_deadline(thread_t *thread, cpu_t *cpu) {
    /* deadline 偏移未按权重缩放 (真 EEVDF 为 ve + slice/w) —
     * eligible 集内排序近似 vruntime 序, 有意简化 */
    uint64_t virtual_slice = cpu->base_quantum;
    uint64_t max_lag = virtual_slice;
    uint64_t avg_vr = cpu->avg_vruntime;
    uint64_t target_vr = (avg_vr > max_lag) ? (avg_vr - max_lag) : 0;

    if (thread->vruntime < target_vr) {
        thread->vruntime = target_vr;
        thread->vruntime_rem = 0;
    } else if (thread->vruntime > avg_vr + max_lag * 2) {
        thread->vruntime = avg_vr + max_lag * 2;
        thread->vruntime_rem = 0;
    }
    thread->deadline = thread->vruntime + virtual_slice;
    thread->min_vruntime_subtree = thread->vruntime;
}

static inline void update_min_vruntime_upward(rb_node_t *node) {
    while (likely(node)) {
        if (likely(node->parent)) PREFETCH_R(node->parent);
        if (node->left)  PREFETCH_R(node->left);
        if (node->right) PREFETCH_R(node->right);
        thread_t *t = rb_to_thread(node);
        uint64_t min_vr = t->vruntime;
        if (node->left) {
            thread_t *lt = rb_to_thread(node->left);
            if (lt->min_vruntime_subtree < min_vr) min_vr = lt->min_vruntime_subtree;
        }
        if (node->right) {
            thread_t *rt = rb_to_thread(node->right);
            if (rt->min_vruntime_subtree < min_vr) min_vr = rt->min_vruntime_subtree;
        }
        if (likely(t->min_vruntime_subtree == min_vr)) break;
        t->min_vruntime_subtree = min_vr;
        node = node->parent;
    }
}

static inline thread_t *first_runnable(rb_node_t *root) {
    for (rb_node_t *node = rb_first(root); likely(node); ) {
        rb_node_t *nx = rb_next(node);
        if (likely(nx)) PREFETCH_R(nx);
        thread_t *t = rb_to_thread(node);
        if (likely(t->state == THREAD_RUNNING)) return t;
        node = nx;
    }
    return nullptr;
}

namespace Schedule {
    namespace Internal {
        void RemoveFromQueue(cpu_t *cpu, thread_t *thread) {
            if (unlikely(!thread->on_rq)) return;
            rb_node_t *node = &thread->rb_node;

            rb_erase(&cpu->runqueue_root, node);
            thread->on_rq = false;
            cpu->thread_count--;
            cpu->total_weight -= thread->weight;

            if (likely(node->parent)) {
                update_min_vruntime_upward(node->parent);
            }

            if (cpu->thread_count == 1) cpu->has_surplus = false;
            if (cpu->thread_count == 0) cpu->has_runnable_thread = false;
        }

        void InsertToQueue(cpu_t *cpu, thread_t *thread) {
            if (unlikely(thread->on_rq)) return;
            calibrate_and_set_deadline(thread, cpu);
            rb_insert(&cpu->runqueue_root, &thread->rb_node, thread_rb_cmp);
            thread->on_rq = true;
            cpu->thread_count++;
            cpu->total_weight += thread->weight;
            if (cpu->thread_count == 1) cpu->has_runnable_thread = true;
            if (cpu->thread_count == 2) cpu->has_surplus = true;
            update_min_vruntime_upward(&thread->rb_node);
        }

        /* ============================================================
         *  锁纪律与无死锁证明
         *
         *  全调度器仅三处阻塞获取 sched_lock, 且获取时均不持有任何
         *  其他 sched_lock:
         *    1. Switch:      自己的锁 (TryPush/Steal 都在其释放后调用);
         *    2. TryPush:     pair 中较低 id 的锁 (第一把); 第二把 trylock;
         *    3. StealThread: victim 锁先释放, 再阻塞取自己的锁 —
         *                    此刻它不持有任何锁.
         *  → hold-and-wait 对阻塞获取不存在 → 等待环不可能 → 无死锁.
         *  所有临界区内部无阻塞调用, 阻塞等待的时长上界 = 最长单个
         *  临界区 (TryPush 双锁段 ≤ 8 次 rb 迁移, 微秒级).
         *  StealThread 的调用者是刚 Pick 不到线程、马上要转 idle 的
         *  CPU, 由它承担这个有界等待是零成本的.
         *  压测验证点: ① sched_lock 最大持锁时长 (临界区前后取 TSC);
         *  ② steal_attempts/thread_steals 比值; ③ 最坏 Pick→dispatch
         *  延迟. 三者有界即与证明一致.
         * ============================================================ */
        thread_t *StealThread(cpu_t *cpu) {
            uint32_t *skip = &per_cpu_steal_throttle[cpu->id].skip;
            if (likely(++(*skip) < SCHED_STEAL_THROTTLE)) return nullptr;
            *skip = 0;

            uint32_t my_mask = cpu_simd_mask(cpu);
            const uint32_t ncpu = (uint32_t)smp_last_cpu + 1;
            uint32_t start_cpu = (sched_pid + PIT::TimeSinceBootMS()
                                + per_cpu_steal_cursor[cpu->id].v) % ncpu;
            per_cpu_steal_cursor[cpu->id].v++;

            for (int pass = 0; pass < 2; pass++) {
                for (uint32_t k = 0; k < ncpu; k++) {
                    uint32_t i = (start_cpu + k) % ncpu;
                    cpu_t *victim = smp_cpu_list[i];
                    if (likely(k + 1 < ncpu))
                        PREFETCH_R(smp_cpu_list[(start_cpu + k + 1) % ncpu]);
                    if (unlikely(!victim || victim == cpu)) continue;
                    if (pass == 0 && cpu_simd_mask(victim) != my_mask) continue;
                    if (unlikely(!atomic_load_1(&victim->has_surplus, ATOMIC_RELAXED))) continue;

                    cpu->sched_stats.steal_attempts++;

                    uint64_t rflags1 = 0;
                    int retries = 0;
                    /* trylock 重试上限: 恰好 100 次尝试, 失败即放弃 */
                    while (unlikely(!spin_trylock_irqsave(&victim->sched_lock, &rflags1))) {
                        if (unlikely(++retries >= 100)) break;
                        asm volatile("pause");
                    }
                    if (unlikely(retries >= 100)) continue;

                    if (victim->thread_count <= 1) {
                        spin_unlock_irqrestore(&victim->sched_lock, rflags1);
                        continue;
                    }

                    thread_t * const victim_curr  = victim->current_thread;
                    thread_t * const victim_idle  = victim->idle_thread;
                    /* 无需饥饿过滤: 入队钳位保证队列内 vruntime ≤ avg+2q
                     * 且 avg 单调递增 — 从队尾 (最不紧迫) 取即可 */

                    thread_t *stolen_batch[SCHED_STEAL_BATCH];
                    int stolen_count = 0;
                    rb_node_t *node = rb_last(victim->runqueue_root.node);
                    while (likely(node && stolen_count < SCHED_STEAL_BATCH)) {
                        if (unlikely(victim->thread_count <= 1)) break;
                        rb_node_t *prev_node = rb_prev(node);
                        if (likely(prev_node)) PREFETCH_R(prev_node);
                        thread_t *stolen = rb_to_thread(node);
                        if (unlikely(stolen == victim_curr)) { node = prev_node; continue; }
                        if (unlikely(stolen == victim_idle))  { node = prev_node; continue; }
                        if (unlikely(stolen->state != THREAD_RUNNING)) { node = prev_node; continue; }
                        /* 有挂起定时器的线程与特定 CPU 的定时器桶绑定,
                         * 迁走会孤儿化定时器 */
                        if (unlikely(stolen->timer_bucket != nullptr)) { node = prev_node; continue; }

                        RemoveFromQueue(victim, stolen);
                        stolen_batch[stolen_count++] = stolen;
                        node = prev_node;
                    }

                    if (likely(stolen_count > 0)) {
                        for (int j = 0; j < stolen_count; j++) {
                            /* TRANSFER: 线程不在任何队列的迁移窗口,
                             * 唤醒者/定时器不会误操作 */
                            __atomic_store_n(&stolen_batch[j]->state, THREAD_TRANSFER, __ATOMIC_RELEASE);
                            PREFETCH_W(stolen_batch[j]);
                        }
                        spin_unlock_irqrestore(&victim->sched_lock, rflags1);

                        /* 阻塞获取, 但此刻不持有任何锁 (victim 已释放) */
                        uint64_t rflags2 = spin_lock_irqsave(&cpu->sched_lock);
                        for (int j = 0; j < stolen_count; j++) {
                            stolen_batch[j]->cpu_num = cpu->id;
                            stolen_batch[j]->timer_cpu = cpu->id;
                            __atomic_store_n(&stolen_batch[j]->state, THREAD_RUNNING, __ATOMIC_RELEASE);
                            InsertToQueue(cpu, stolen_batch[j]);
                        }
                        thread_t *best = Pick(cpu);
                        spin_unlock_irqrestore(&cpu->sched_lock, rflags2);

                        if (likely(best)) {
                            cpu->sched_stats.thread_steals += stolen_count;
                            return best;
                        }
                        return nullptr;
                    }
                    spin_unlock_irqrestore(&victim->sched_lock, rflags1);
                }
            }
            return nullptr;
        }

        void TryPush(cpu_t *cpu) {
            cpu->sched_stats.try_pushes++;

            if (unlikely(!atomic_load_1(&cpu->has_surplus, ATOMIC_RELAXED))) return;
            if (cpu->thread_count < 2) return;
            uint64_t my_weight = cpu->total_weight + (cpu->current_thread ? cpu->current_thread->weight : 0);

            uint32_t my_mask = cpu_simd_mask(cpu);
            cpu_t *target = nullptr;
            uint64_t target_weight = UINT64_MAX;
            /* fallback (异 SIMD 掩码) 权重必须随指针一起记录 — 只存
             * 指针不存权重的话, 纯 fallback 场景下 target_weight 保持
             * UINT64_MAX 哨兵, gap 检查恒为真 → 跨掩码推送死路径.
             * 最小值追踪, 与同掩码路径语义一致. */
            cpu_t *fallback_target = nullptr;
            uint64_t fallback_weight = UINT64_MAX;

            const int32_t last = smp_last_cpu;
            for (int32_t i = 0; i <= last; i++) {
                cpu_t *other = smp_cpu_list[i];
                if (likely(i < last)) PREFETCH_R(smp_cpu_list[i + 1]);
                if (unlikely(!other || other == cpu)) continue;
                uint64_t ow = other->total_weight + (other->current_thread ? other->current_thread->weight : 0);
                if (ow < my_weight && ow < target_weight) {
                    if (cpu_simd_mask(other) == my_mask) { target = other; target_weight = ow; }
                    else if (ow < fallback_weight) { fallback_target = other; fallback_weight = ow; }
                }
            }

            bool cross_mask_push = false;
            if (unlikely(!target)) {
                if (fallback_target) {
                    target = fallback_target;
                    target_weight = fallback_weight;
                    cross_mask_push = true;
                }
                else return;
            }

            /* 门槛 = target×(1+1/2^shift), 移位越小门槛越高.
             * 跨掩码迁移成本更高 → >>1 (50% 失衡), 同掩码 >>2 (25%).
             * 注意方向: 加大移位是放宽不是收紧. */
            const uint32_t gap_shift = cross_mask_push ? 1 : SCHED_PUSH_GAP_SHIFT;
            if (unlikely(target_weight + (target_weight >> gap_shift) >= my_weight)) return;

            cpu_t *lock_a = (cpu->id < target->id) ? cpu : target;
            cpu_t *lock_b = (cpu->id < target->id) ? target : cpu;

            uint64_t rflags = spin_lock_irqsave(&lock_a->sched_lock);
            uint64_t rflags_b;
            if (unlikely(!spin_trylock_irqsave(&lock_b->sched_lock, &rflags_b))) {
                spin_unlock_irqrestore(&lock_a->sched_lock, rflags);
                return;
            }

            thread_t * const my_curr = cpu->current_thread;
            thread_t * const my_idle = cpu->idle_thread;

            int push_count = 0;
            rb_node_t *node = rb_last(cpu->runqueue_root.node);
            while (likely(node && push_count < SCHED_STEAL_BATCH)) {
                uint64_t tc_w = target->total_weight + (target->current_thread ? target->current_thread->weight : 0);
                uint64_t mc_w = cpu->total_weight + (cpu->current_thread ? cpu->current_thread->weight : 0);
                if (tc_w >= mc_w) break;

                rb_node_t *prev_node = rb_prev(node);
                if (likely(prev_node)) PREFETCH_R(prev_node);
                thread_t *to_push = rb_to_thread(node);
                if (unlikely(to_push == my_curr)) { node = prev_node; continue; }
                if (unlikely(to_push == my_idle)) { node = prev_node; continue; }
                if (unlikely(to_push->timer_bucket != nullptr)) { node = prev_node; continue; }

                __atomic_store_n(&to_push->state, THREAD_TRANSFER, __ATOMIC_RELEASE);
                RemoveFromQueue(cpu, to_push);
                to_push->cpu_num = target->id;
                to_push->timer_cpu = target->id;
                __atomic_store_n(&to_push->state, THREAD_RUNNING, __ATOMIC_RELEASE);
                InsertToQueue(target, to_push);
                push_count++;
                node = prev_node;
            }

            if (unlikely(cpu->thread_count < 2)) cpu->has_surplus = false;

            cpu->sched_stats.push_success += push_count;
            spin_unlock_irqrestore(&lock_b->sched_lock, rflags_b);
            spin_unlock_irqrestore(&lock_a->sched_lock, rflags);

            /* 推送成功且目标空闲 → IPI 立刻唤醒, 不等目标的 idle 量子.
             * 跨掩码推送涉及 fx_area 在不同 XsaveMask 的 CPU 间保存/
             * 加载, 混合掩码系统上需实测该路径 */
            if (unlikely(push_count > 0)) {
                thread_t *tcurr = __atomic_load_n(&target->current_thread, __ATOMIC_ACQUIRE);
                if (tcurr == target->idle_thread) {
                    LAPIC::IPI(target->lapic_id, SCHED_VEC);
                }
            }
        }

        thread_t *Pick(cpu_t *cpu) {
            rb_node_t *root = cpu->runqueue_root.node;
            if (unlikely(!root)) return nullptr;

            const uint64_t avg_vr = cpu->avg_vruntime;
            thread_t *root_t = rb_to_thread(root);
            thread_t *best = nullptr;

            /* 整树无 eligible → 直接取最左 (deadline 最小) 兜底,
             * 防饥饿 */
            if (unlikely(root_t->min_vruntime_subtree > avg_vr)) {
                best = first_runnable(root);
                if (likely(best)) RemoveFromQueue(cpu, best);
                return best;
            }

            /* 下降搜索: 树按 deadline 排序, 增广字段使 "子树是否含
             * eligible" 成为 O(1) 判断 → O(log n) 找到
             * eligible 且 deadline 最小的节点 */
            rb_node_t *node = root;
            while (likely(node)) {
                rb_node_t *l = node->left;
                if (likely(l)) {
                    PREFETCH_R(l);
                    if (l->right) PREFETCH_R(l->right);
                    thread_t *lt = rb_to_thread(l);
                    if (lt->min_vruntime_subtree <= avg_vr) {
                        node = l;
                        continue;
                    }
                }
                thread_t *cur = rb_to_thread(node);
                if (cur->vruntime <= avg_vr) {
                    if (likely(cur->state == THREAD_RUNNING)) { best = cur; break; }
                    node = node->right;
                    if (likely(node)) PREFETCH_R(node);
                    continue;
                }
                node = node->right;
                if (likely(node)) PREFETCH_R(node);
            }

            if (unlikely(!best)) best = first_runnable(cpu->runqueue_root.node);
            if (likely(best)) RemoveFromQueue(cpu, best);
            return best;
        }

        void Switch(context_t *ctx) {
            LAPIC::StopTimer();
            cpu_t *cpu = this_cpu();
            if (unlikely(!cpu)) {
                /* 早退也要 EOI, 否则 LAPIC ISR 位悬挂, 阻断后续中断 */
                LAPIC::EOI();
                return;
            }
            uint64_t rflags = irq_save();

            /* Early SMP bring-up window: an AP arms its first LAPIC tick and
               sti inside smp_cpu_init() before Schedule::Install() creates its
               idle_thread and binds current_thread. There is nothing to schedule
               yet, so rearm the timer and return. Latch current to idle as soon
               as the idle thread exists. */
            if (unlikely(!cpu->idle_thread || !cpu->current_thread)) {
                if (cpu->idle_thread) cpu->current_thread = cpu->idle_thread;
                LAPIC::Oneshot(SCHED_VEC, cpu->base_quantum * cpu->lapic_ticks);
                LAPIC::EOI();
                irq_restore(rflags);
                return;
            }

            /* 抢占被禁: 只重臂返回. 不消费 need_resched/yield 标志 —
             * 在此处清掉的话请求会被无声吞掉; 标志保持锁存,
             * CheckPreempt 在计数归零后重新触发 SCHED_VEC. */
            if (unlikely(cpu->preempt_count > 1)) {
                if (likely(cpu->current_thread)) {
                    uint64_t q = get_dynamic_quantum(cpu, cpu->current_thread);
                    LAPIC::Oneshot(SCHED_VEC, q * cpu->lapic_ticks);
                }
                LAPIC::EOI();
                irq_restore(rflags);
                return;
            }

            if (unlikely(__atomic_load_n(&need_resched_flags[cpu->id], __ATOMIC_ACQUIRE))) {
                __atomic_store_n(&need_resched_flags[cpu->id], false, __ATOMIC_RELEASE);
            }

            /* Voluntary yield? Consume the flag so the lockless fast path
               below cannot skip the only other runnable thread. */
            const bool yield_req = __atomic_exchange_n(
                &yield_request_flags[cpu->id], false, __ATOMIC_ACQ_REL);

            cpu->tick_count++;
            uint64_t now = PIT::TimeSinceBootMS();
            uint64_t cur_tsc = sced_rdtsc();

            bool curr_invalid = false;
            thread_t *curr_thread = safe_get_current_thread(cpu, curr_invalid);
            const bool curr_is_idle = (curr_thread == cpu->idle_thread);

            /* ctx/SIMD 保存 — 锁外 (这些字段只有运行该线程的核会碰,
             * 迁移用 THREAD_TRANSFER 窗口保证无人触碰) */
            if (likely(curr_thread && !curr_is_idle)) {
                curr_thread->fs = rdmsr(FS_BASE);
                curr_thread->ctx = *ctx;
                if (unlikely(curr_thread->fx_area)) {
                    cpu->OverLoadableFuncs.StoreSIMDState(curr_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
                }
            }

            /* 账本结算 — 锁外 */
            dynamic_adjust_quantum(cpu, curr_thread, now, cur_tsc);

            /* 实际运行时长 — 由 last_run_time 结算 (而非编程量子).
             * delta==0 强制为 1: 亚毫秒窗口 (被唤醒抢占提前打断) 的
             * 系统性低估由 riprate_update 的最小采样门兜底. */
            uint64_t last_slice_ms = 0;

            if (likely(curr_thread && !curr_is_idle)) {
                uint64_t delta = now - curr_thread->last_run_time;
                curr_thread->last_run_time = now;
                if (unlikely(delta == 0)) delta = 1;
                last_slice_ms = delta;
                /* vruntime 单位: weight-1024 下的 1ms. 余数进位保证
                 * 长跑精确, 轻线程不被整型除法系统性少记. */
                uint64_t w = likely(curr_thread->weight) ? curr_thread->weight : 1024;
                uint64_t vruntime_total = delta * 1024 + curr_thread->vruntime_rem;
                uint64_t vruntime_delta = vruntime_total / w;
                curr_thread->vruntime_rem = vruntime_total % w;
                curr_thread->vruntime += vruntime_delta;

                /* 虚拟时钟: 负载相关速率 (所有可运行权重的加权),
                 * 稳态下各线程 vruntime 收敛到时钟附近 → 份额=权重比 */
                uint64_t active_weight = cpu->total_weight + w;
                uint64_t avg_total = delta * 1024 + cpu->avg_vruntime_rem;
                uint64_t avg_delta = avg_total / active_weight;
                cpu->avg_vruntime_rem = avg_total % active_weight;
                cpu->avg_vruntime += avg_delta;
            } else if (unlikely(curr_is_idle)) {
                /* 空闲按 1024 权重推进时钟 (≈真实时间), 维持跨核可比 */
                uint64_t delta = now - curr_thread->last_run_time;
                curr_thread->last_run_time = now;
                if (likely(delta > 0)) {
                    const uint64_t base_weight = 1024;
                    uint64_t avg_total = delta * 1024 + cpu->avg_vruntime_rem;
                    uint64_t avg_delta = avg_total / base_weight;
                    cpu->avg_vruntime_rem = avg_total % base_weight;
                    cpu->avg_vruntime += avg_delta;
                }
            }

            /* RIP 反馈采样 — 锁外 (cs/TSC 供信号层) */
            riprate_update(cpu, curr_thread, ctx->rip, ctx->cs,
                           last_slice_ms, cur_tsc, now);

            /* 免锁重入判定 (ZOMBIE 强制慢路径) */
            bool need_lock = true;
            uint32_t curr_state_snap = curr_thread ? curr_thread->state : 0xFFFFFFFF;

            if (likely(curr_thread && !curr_is_idle && !yield_req) &&
                likely(curr_state_snap == THREAD_RUNNING)) {
                /* Lockless fast path ONLY when no other thread is queued.
                   thread_count excludes the running curr, so ==0 means there
                   is truly no competitor. A count of 1 means one waiter is
                   ready: the timer tick MUST take the slow path so EEVDF can
                   preempt (==1 as lockless starved the sole peer ~14s). */
                if (unlikely(cpu->thread_count == 0)) {
                    need_lock = false;
                }
            } else if (unlikely(curr_is_idle && cpu->thread_count == 0)) {
                need_lock = false;
            }

            thread_t *next_thread = curr_thread;
            thread_t *zombie_to_free = nullptr;

            if (likely(need_lock)) {
                uint64_t sflags = spin_lock_irqsave(&cpu->sched_lock);

                if (unlikely(curr_invalid)) {
                    cpu->current_thread = curr_thread;
                }

                /* 僵尸批量搬出: kfree 昂贵, 不碰锁; 每次有界 */
                if (unlikely(cpu->zombie_count >= ZOMBIE_RECLAIM_THRESHOLD)) {
                    int moved = 0;
                    thread_t *z = cpu->zombie_list;
                    while (likely(z && moved < ZOMBIE_RECLAIM_BATCH)) {
                        thread_t *next = z->zombie_next;
                        if (likely(next)) PREFETCH_R(next);
                        z->zombie_next = zombie_to_free;
                        zombie_to_free = z;
                        z = next;
                        moved++;
                    }
                    if (likely(zombie_to_free)) {
                        cpu->zombie_list = z;
                        cpu->zombie_count -= moved;
                    }
                }

                uint32_t curr_state = curr_thread ? curr_thread->state : 0xFFFFFFFF;
                if (unlikely(curr_thread && curr_state == THREAD_ZOMBIE && !curr_is_idle)) {
                    curr_thread->zombie_next = cpu->zombie_list;
                    cpu->zombie_list = curr_thread;
                    cpu->zombie_count++;
                    curr_thread = nullptr;
                } else if (likely(curr_thread && curr_state == THREAD_RUNNING && !curr_is_idle)) {
                    InsertToQueue(cpu, curr_thread);
                }

                next_thread = Pick(cpu);

                spin_unlock_irqrestore(&cpu->sched_lock, sflags);
            }

            reclaim_zombie_list(cpu, zombie_to_free);

            irq_restore(rflags);

            if (unlikely((cpu->tick_count & 0xFF) == 0) &&
                likely(cpu->thread_count > 2)) {
                TryPush(cpu);
            }

            if (unlikely(!next_thread)) {
                next_thread = StealThread(cpu);
                if (unlikely(!next_thread)) next_thread = cpu->idle_thread;
            }

            rflags = irq_save();

            uint64_t quantum = cpu->base_quantum;
            const bool is_switch = (next_thread != curr_thread);

            if (unlikely(!is_switch)) {
                if (likely(next_thread != cpu->idle_thread)) {
                    quantum = get_dynamic_quantum(cpu, next_thread);
                }
                LAPIC::Oneshot(SCHED_VEC, quantum * cpu->lapic_ticks);
                LAPIC::EOI();
                irq_restore(rflags);
                return;
            }

            /* ---- 真正的上下文切换 ----
             * ctx 指向中断栈上的寄存器现场: 上面已把当前线程现场存入
             * 其 thread 结构, 此处覆写为 next 的现场, iret 返回时即
             * "返回进" next — 用户态可见的通用寄存器切换零汇编.
             * 只需手工处理中断帧装不下的状态: TSS 内核栈、CR3、
             * FS base、xsave. */
            __atomic_store_n(&cpu->current_thread, next_thread, __ATOMIC_RELEASE);
            cpu->sched_stats.context_switches++;
            next_thread->last_run_time = now;

            PREFETCH_RH(&next_thread->ctx);
            PREFETCH_RH(&next_thread->kernel_rsp);
            if (unlikely(next_thread->fx_area)) PREFETCH_RH(next_thread->fx_area);

            *ctx = next_thread->ctx;
            TSS::SetRSP(cpu->id, 0, (void*)next_thread->kernel_rsp);
            cpu->kernel_stack = next_thread->kernel_rsp;

            if (unlikely(!curr_thread || curr_thread->pagemap != next_thread->pagemap)) {
                VMM::SwitchPageMap(next_thread->pagemap);
            }

            cpu->OverLoadableFuncs.WRFSBASE(next_thread->fs);
            if (unlikely(next_thread->fx_area)) {
                cpu->OverLoadableFuncs.LoadSIMDState(next_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
            }

            /* RIP 窗口起点快照: 连同信号标签一起初始化, 首个采样窗口
             * 从 dispatch 起就是可比的. rip_last_tsc 与 dispatch 快照
             * 配套 (否则首窗口 TSC 差分横跨上次运行的陈旧值); 运行中
             * 的 pagemap/ring 切换由 tag 失效路径接管 — 两者是不相交
             * 的路径, 都必要. rip_last_sample_ms 刻意不在此重置:
             * 离 CPU 时长必须进入老化计算. */
            next_thread->dispatch_rip = next_thread->ctx.rip;
            next_thread->rip_signal_tag =
                (uint64_t)next_thread->pagemap ^ (uint64_t)(next_thread->ctx.cs & 3);
            next_thread->rip_last_tsc = cur_tsc;

            quantum = (likely(next_thread != cpu->idle_thread))
                    ? get_dynamic_quantum(cpu, next_thread)
                    : cpu->base_quantum;

            LAPIC::Oneshot(SCHED_VEC, quantum * cpu->lapic_ticks);
            LAPIC::EOI();
            irq_restore(rflags);
        }
    }

    /* 反馈统计导出 — 接到现有 proc/sysinfo/debug 接口上.
     * 跨核读取是近似快照 (无锁) — 调试用途足够.
     * 边界用 smp_last_cpu: 落在 (smp_last_cpu, MAX_CPU) 区间的
     * id 读到的是 BSS 全零, 返回 true 会误导. */
    bool GetRipStats(uint32_t cpu_id, uint64_t out[14]) {
        if (unlikely(cpu_id >= MAX_CPU || cpu_id > (uint32_t)smp_last_cpu)) return false;
        rip_stats_ctx *st = &rip_stats[cpu_id];
        out[0]  = st->samples;      out[1]  = st->outliers;
        out[2]  = st->tag_invalid;  out[3]  = st->short_windows;
        out[4]  = st->stalled;      out[5]  = st->dead_zone;
        out[6]  = st->sat_hi;       out[7]  = st->sat_lo;
        out[8]  = st->base_resets;  out[9]  = st->tsc_mismatch;
        out[10] = st->mean_abs_dev;
        out[11] = __atomic_load_n(&rip_fast_weight[cpu_id].w, __ATOMIC_RELAXED);
        out[12] = st->near_reset;   out[13] = st->wf_clamped;
        return true;
    }

    void CheckPreempt(context_t *ctx) {
        (void)ctx;
        cpu_t *cpu = this_cpu();
        if (unlikely(!cpu)) return;
        if (unlikely(__atomic_load_n(&need_resched_flags[cpu->id], __ATOMIC_ACQUIRE)) && cpu->preempt_count == 0) {
            asm volatile("int %0" :: "i"(SCHED_VEC));
        }
    }

    void TriggerPreempt(thread_t *woked_thread) {
        if (unlikely(!woked_thread)) return;
        uint32_t cpu_num = __atomic_load_n(&woked_thread->cpu_num, __ATOMIC_ACQUIRE);
        if (unlikely(cpu_num >= MAX_CPU)) return;
        cpu_t *cpu = smp_cpu_list[cpu_num];
        if (unlikely(!cpu)) return;

        thread_t *curr = __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE);
        if (unlikely(!curr || curr == cpu->idle_thread)) {
            LAPIC::IPI(cpu->lapic_id, SCHED_VEC);
            return;
        }

        PREFETCH_RH(woked_thread);
        PREFETCH_RH(curr);

        if (woked_thread->vruntime <= cpu->avg_vruntime && woked_thread->deadline < curr->deadline) {
            /* 剩余虚拟 slice 防下溢 (deadline 是入队时刻的, 运行中
             * vruntime 已前进). 剩余不足 1/4 (ceil) 就不打断 — 省一
             * 次上下文切换, 延迟代价 ≤ 1/4 实际 slice. vruntime 是
             * 全局单位, 该分数对任意权重都对应实际 slice 的同一
             * 比例. */
            uint64_t remaining_vr = (curr->deadline > curr->vruntime)
                                  ? (curr->deadline - curr->vruntime) : 0;
            if (likely(remaining_vr < (((uint64_t)cpu->base_quantum + 3) >> 2))) {
                return;
            }

            cpu_t *cur_cpu = this_cpu();
            if (likely(cpu != cur_cpu)) {
                LAPIC::IPI(cpu->lapic_id, SCHED_VEC);
            } else {
                __atomic_store_n(&need_resched_flags[cpu->id], true, __ATOMIC_RELEASE);
            }
        }
    }

    void Init() {
        /* per-CPU 融合权重显式初始化 (静态零 + 读取端防御双保险) */
        for (uint32_t i = 0; i < MAX_CPU; i++) {
            __atomic_store_n(&rip_fast_weight[i].w, RIP_FAST_W_NORM_Q10, __ATOMIC_RELAXED);
        }
        if (unlikely(!pid2proc_tree)) {
            pid2proc_tree = (art_tree*)kmalloc(sizeof(art_tree));
            if (unlikely(art_tree_init(pid2proc_tree) != 0)) Panic("ART TREE INIT FAILED!");
        }
        if (unlikely(!NOT_RUNQ_P)) {
            NOT_RUNQ_P = (art_tree*)kmalloc(sizeof(art_tree));
            if (unlikely(art_tree_init(NOT_RUNQ_P) != 0)) Panic("ART TREE INIT FAILED!");
        }
        idt_install_irq(SCHED_VEC, (void*)Schedule::Internal::Switch);
        idt_set_ist(SCHED_VEC, 0);
    }

    void Install() {
        const uint32_t last = (uint32_t)smp_last_cpu;
        for (uint32_t i = 0; i <= last; i++) {
            cpu_t *cpu = smp_cpu_list[i];
            if (unlikely(!cpu)) continue;
            if (likely(i < last)) PREFETCH_R(smp_cpu_list[i + 1]);
            cpu->timer_last_tick = PIT::TimeSinceBootMS();
            proc_t *proc = Schedule::NewProcess(false);
            thread_t *idle_t = Schedule::NewKernelThread(proc, cpu->id, 15, (void*)sched_idle);
            uint64_t rflags = spin_lock_irqsave(&cpu->sched_lock);
            Internal::RemoveFromQueue(cpu, idle_t);
            spin_unlock_irqrestore(&cpu->sched_lock, rflags);
            cpu->idle_thread = idle_t;
            /* APs finish smp_cpu_init() with current_thread still NULL (that
               boot path never assigns it), so their first Switch would trip
               on a null current. Bind every not-yet-running CPU to its own
               idle thread. The BSP already owns init_thread; a live current
               must never be clobbered. */
            if (cpu->current_thread == nullptr)
                cpu->current_thread = idle_t;
            idle_t->last_run_time = PIT::TimeSinceBootMS();

            dyn_ctx[i].last_adjust_ms = PIT::TimeSinceBootMS();
            dyn_ctx[i].last_ctx_sw = cpu->sched_stats.context_switches;
        }
        atomic_store_8((volatile uint8_t*)&PIT::TickHandle, (uint64_t)(uintptr_t)&PIT::Tick_, 0);
    }

    thread_t* this_thread() { cpu_t* cpu = this_cpu(); return likely(cpu) ? cpu->current_thread : nullptr; }
    proc_t *this_proc() { thread_t* t = this_thread(); return likely(t) ? t->parent : nullptr; }
    void Yield() {
        cpu_t *yc = this_cpu();
        if (yc) __atomic_store_n(&yield_request_flags[yc->id], true, __ATOMIC_RELEASE);
        LAPIC::StopTimer();
        asm volatile("int %0" :: "i"(SCHED_VEC));
    }
    void PAUSE() { LAPIC::StopTimer(); }
    void Resume() {
        cpu_t* cur_cpu = this_cpu();
        if (unlikely(!cur_cpu)) return;
        const int32_t last = smp_last_cpu;
        for (int32_t i = 0; i <= last; i++) {
            cpu_t *c = smp_cpu_list[i];
            if (likely(i < last)) PREFETCH_R(smp_cpu_list[i + 1]);
            if (likely(c && i != cur_cpu->id)) LAPIC::IPI(c->lapic_id, SCHED_VEC);
        }
        LAPIC::IPI(cur_cpu->lapic_id, SCHED_VEC);
    }
}