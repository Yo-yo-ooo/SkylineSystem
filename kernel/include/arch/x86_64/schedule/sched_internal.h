// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* sched_internal.h — sched.cpp(核心) 与 sched_rip.cpp(RIP 反馈/量子层)
   共享的内部声明 (拆分自 sched.cpp) */
#pragma once

#include <arch/x86_64/schedule/sched.h>
#include <atomic/atomic.h>

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
struct alignas(64) rip_fast_weight_ctx { volatile uint32_t w; char pad[60]; };
extern rip_fast_weight_ctx rip_fast_weight[MAX_CPU];

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
extern rip_stats_ctx rip_stats[MAX_CPU];

/* 可选 TSC 校准钩子 — 返回 TSC 每毫秒周期数 (默认弱符号 = 未校准) */
uint64_t sched_tsc_per_ms(void);

/* SMP 均衡游标 (核心 StealThread 使用) */
struct alignas(64) sched_steal_throttle { uint32_t skip; char pad[60]; };
extern sched_steal_throttle per_cpu_steal_throttle[MAX_CPU];
struct alignas(64) sched_padded_u32 { uint32_t v; };
extern sched_padded_u32 per_cpu_steal_cursor[MAX_CPU];

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
extern dyn_adjust_ctx dyn_ctx[MAX_CPU];

/* 量子层入口 (核心 tick 处理器调用) */
void        dynamic_adjust_quantum(cpu_t *cpu, thread_t *curr_thread, uint64_t now_ms, uint64_t cur_tsc);
uint64_t    get_dynamic_quantum(cpu_t *cpu, thread_t *thread);
uint64_t    eligibility_capped_quantum(cpu_t *cpu, thread_t *t);
void        riprate_update(cpu_t *cpu, thread_t *thread, uint64_t rip, uint64_t cs,
                           uint64_t last_slice_ms, uint64_t cur_tsc, uint64_t now_ms);

/* 跨单元共享的进程管理 extern (核心与 RIP 层都用) */
extern art_tree *pid2proc_tree;
extern art_tree *NOT_RUNQ_P;
extern spinlock_t PID2PROC_TREE_LOCK;
extern spinlock_t PROC_LIST_LOCK;
extern uint64_t sched_pid;
extern uint64_t sched_tid;
