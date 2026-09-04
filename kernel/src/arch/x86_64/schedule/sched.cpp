// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// sched.cpp - Rate-aware EEVDF (REEVDF) Schedule ALGO IMPL

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
/*  WAIT_THREAD_TIMEOUT_MS (从未使用) / SCHED_HUNGER_THRESHOLD (死代码) /
       PREEMPT_THRESHOLD (量纲错误, 被 TriggerPreempt 的 slice 分数替代) 已删除 */
#define SCHED_STEAL_THROTTLE 8
#define SCHED_PUSH_GAP_SHIFT 2


/* ============================================================
 *  RIP 速率反馈 — 全部定点 Q10 (1.0x = 1024)
 * ============================================================ */
#define RIPRATE_FRAC_BITS  10
/* v3 FIX: Q10 下 1.0x = 1<<10. 旧值 1024<<10 是 Q20 量纲 */
#define RIPRATE_INIT       (1ULL << RIPRATE_FRAC_BITS)      /* 1.0x */
#define RIPRATE_ONE        (1ULL << RIPRATE_FRAC_BITS)      /* 1.0x 常量 */
#define RIPRATE_SHIFT      3                                 /* EWMA 1/8 */
#define RIPRATE_MAX_MULT   (4ULL  << RIPRATE_FRAC_BITS)     /* 4x 上限 */
#define RIPRATE_MIN_MULT   (1ULL << (RIPRATE_FRAC_BITS - 2))/* 0.25x 下限 = 256 */
/*  观测值超过 16x 时钳位 (而非丢弃) — 丢弃会让慢基线永远追不上快线程 */
#define RIPRATE_OUTLIER_MULT (16ULL << RIPRATE_FRAC_BITS)

/*  直接修正项的钳位 (± 量子偏移上限, 单位: LAPIC tick 基准的量子单位) */
#define RIPADJ_MAX        4                                  /* 最多 +4 */
#define RIPADJ_MIN       (-4)                                /* 最多 -4 */
/*  老化阈值 — 超过这个 ms 没采样, 倍率向 1.0 收缩一步 */
#define RIPRATE_AGING_MS 50ULL
/*  衰减步长 1/16 (位移 4) */
#define RIPRATE_DECAY_SHIFT 4

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
                /* v3 FIX: 高负载 + 切换风暴 → 拉长量子降低切换频率.
                 * 旧代码在这里 quantum--, 方向反了: 切换已经过多,
                 * 缩短量子只会制造更多切换. */
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
 *  get_dynamic_quantum — 倍率乘法 + 直接修正项叠加
 *
 *    eff = (weight_quantum × mult >> FRAC) + rip_quantum_adj
 *
 *  两个通道并存的理由:
 *    乘法通道: 稳态塑形 (长期特征)
 *    修正通道: 快速双向响应 (即时偏差) — adj 有符号, 可正可负
 *
 *   mult == 0 (零初始化、从未采样) 按 1.0x 处理,
 *      防止新线程首个量子被压成 1 tick.
 * ============================================================ */
static inline uint64_t get_dynamic_quantum(cpu_t *cpu, thread_t *thread) {
    if (unlikely(!thread || thread == cpu->idle_thread)) return cpu->base_quantum;

    uint64_t base;
    /*  custom_quantum 作为基准而非硬覆盖 — 反馈仍生效 */
    if (unlikely(thread->custom_quantum > 0)) {
        base = thread->custom_quantum;
    } else {
        uint64_t weight = likely(thread->weight) ? thread->weight : 1024;
        base = (cpu->base_quantum * weight) / 1024;
    }

    /* 乘法通道 ( 零初始化防御) */
    uint64_t mult = likely(thread->rip_rate_mult) ? thread->rip_rate_mult : RIPRATE_ONE;
    uint64_t q = (base * mult) >> RIPRATE_FRAC_BITS;

    /*  直接修正通道 — 有符号加减 */
    int32_t adj = thread->rip_quantum_adj;
    if (likely(adj > 0)) {
        q += (uint64_t)adj;
    } else if (unlikely(adj < 0)) {
        uint64_t sub = (uint64_t)(-(int64_t)adj);
        q = (q > sub) ? (q - sub) : 1;
    }

    if (unlikely(q < 1)) q = 1;
    if (unlikely(q > cpu->base_quantum * 8)) q = cpu->base_quantum * 8;
    return q;
}

static inline void riprate_update(cpu_t *cpu, thread_t *thread,
                                  uint64_t rip_now, uint64_t elapsed_ms,
                                  uint64_t now_ms) {
    if (unlikely(!thread || thread == cpu->idle_thread)) return;

    
    if (unlikely(thread->rip_rate_mult == 0)) thread->rip_rate_mult = RIPRATE_ONE;

    
    uint64_t last_sample = thread->rip_last_sample_ms;
    if (unlikely(last_sample != 0 && (now_ms - last_sample) > RIPRATE_AGING_MS)) {
        /* mult → 1.0 收缩 1/16. 收缩不会越过 1.0 (减量 ≤ 差值),
           原 clamp 范围保持不变, 无需重新钳位 */
        uint64_t m = thread->rip_rate_mult;
        if (likely(m > RIPRATE_ONE)) {
            m -= (m - RIPRATE_ONE) >> RIPRATE_DECAY_SHIFT;
        } else if (unlikely(m < RIPRATE_ONE)) {
            m += (RIPRATE_ONE - m) >> RIPRATE_DECAY_SHIFT;
        }
        thread->rip_rate_mult = m;

        /* adj → 0 收缩.
         * v3 FIX: (x+7)>>4 在 |adj| ≤ 8 时恒为 0, 而 clamp 是 ±4 —
         * 旧代码 adj 永不衰减. (x+15)>>4 = ceil(x/16) 保证 ≥1 步. */
        int32_t adj = thread->rip_quantum_adj;
        if (likely(adj > 0)) {
            adj -= (int32_t)(((uint32_t)adj + 15) >> RIPRATE_DECAY_SHIFT);
        } else if (unlikely(adj < 0)) {
            uint32_t mag = (uint32_t)(-(int64_t)adj);
            adj += (int32_t)((mag + 15) >> RIPRATE_DECAY_SHIFT);
        }
        thread->rip_quantum_adj = adj;
    }
    thread->rip_last_sample_ms = now_ms;

    /* 同毫秒内双 tick: 窗口无效, 只做老化 (见上) 不做速率采样 */
    if (unlikely(elapsed_ms == 0)) return;

    /* ---- 采样: 逐点窗口 ----
     * dispatch_rip 在 dispatch 时初始化, 此处每采样重臂 —
     * 早退路径 (离群/停滞) 也不累积窗口, 下次采样从干净起点开始 */
    uint64_t progress = rip_now - thread->dispatch_rip;
    thread->dispatch_rip = rip_now;

    /* 环绕/异常防御: 紧回跳循环的 RIP 可能倒退 → 无符号回绕成巨值,
     * 该样本丢弃 (窗口已重臂, 不污染下一次) */
    if (unlikely(progress > (1ULL << 44))) return;

    uint64_t obs_rate = progress / elapsed_ms;
    if (unlikely(obs_rate == 0)) return;         /* 完全停滞, 不采样 */

    uint64_t *avg_rate = &cpu->rip_avg_rate;
    if (unlikely(*avg_rate == 0)) {
        *avg_rate = obs_rate;
        return;                                  /* 首采样只建基线 */
    }

    /* 离群钳位 (16x) 而非丢弃. 丢弃的问题: 首采样建了慢基线后,
     * 快线程的观测永远 > 16x 被丢, 基线永远卡在慢速率.
     * 钳位后 EWMA 每采样最多把基线拉高 ~2x, 几个采样即可收敛. */
    uint64_t obs_cap = (*avg_rate * RIPRATE_OUTLIER_MULT) >> RIPRATE_FRAC_BITS;
    if (unlikely(obs_rate > obs_cap)) obs_rate = obs_cap;

    /* 观测倍率 = obs / avg, 定点 */
    uint64_t obs_mult = (obs_rate << RIPRATE_FRAC_BITS) / (*avg_rate);

    /* 核基准 EWMA (v3 FIX: 先比大小再加减, 防无符号回绕) */
    if (likely(obs_rate >= *avg_rate)) {
        *avg_rate += (obs_rate - *avg_rate) >> 3;
    } else {
        *avg_rate -= (*avg_rate - obs_rate) >> 3;
    }

    /* ---- b) 倍率 EWMA (双向, v3 FIX 同上防回绕) ---- */
    uint64_t m = thread->rip_rate_mult;
    if (likely(obs_mult >= m)) {
        m += (obs_mult - m) >> RIPRATE_SHIFT;
    } else {
        m -= (m - obs_mult) >> RIPRATE_SHIFT;
    }
    if (unlikely(m > RIPRATE_MAX_MULT)) m = RIPRATE_MAX_MULT;
    if (unlikely(m < RIPRATE_MIN_MULT)) m = RIPRATE_MIN_MULT;
    thread->rip_rate_mult = m;

    /* ---- c) 直接修正项 (有符号 "减" 通道) ---- */
    /* dev = obs_mult - 1.0, 定点 */
    int64_t dev = (int64_t)obs_mult - (int64_t)RIPRATE_ONE;

    /* 修正步长: dev >> 6 → 每次最多动 dev/64, 温和收敛 */
    int32_t step = (int32_t)(dev >> 6);
    if (unlikely(step == 0)) {
        /* 小偏差也保证至少 ±1 的修正, 否则永远修不动 */
        step = (dev > 0) ? 1 : ((dev < 0) ? -1 : 0);
    }

    int32_t adj = thread->rip_quantum_adj + step;
    if (unlikely(adj > RIPADJ_MAX)) adj = RIPADJ_MAX;
    if (unlikely(adj < RIPADJ_MIN)) adj = RIPADJ_MIN;
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
                    /*  重试上限语义化 — 恰好 100 次尝试, 失败即放弃
                       (旧写法 retries 达 101 才退出, 正确性靠巧合) */
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
                    /*  hunger 检查已删 — 入队时 calibrate clamp 保证
                     * 队列中 vruntime ≤ avg + 2q, 而 avg 单调递增,
                     * "vr > avg + 5M" 永假 (死代码). */

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
                        if (unlikely(stolen->timer_bucket != nullptr)) { node = prev_node; continue; }

                        RemoveFromQueue(victim, stolen);
                        stolen_batch[stolen_count++] = stolen;
                        node = prev_node;
                    }

                    if (likely(stolen_count > 0)) {
                        for (int j = 0; j < stolen_count; j++) {
                            __atomic_store_n(&stolen_batch[j]->state, THREAD_TRANSFER, __ATOMIC_RELEASE);
                            PREFETCH_W(stolen_batch[j]);
                        }
                        spin_unlock_irqrestore(&victim->sched_lock, rflags1);

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
            cpu_t *fallback_target = nullptr;
            uint64_t target_weight = UINT64_MAX;

            const int32_t last = smp_last_cpu;
            for (int32_t i = 0; i <= last; i++) {
                cpu_t *other = smp_cpu_list[i];
                if (likely(i < last)) PREFETCH_R(smp_cpu_list[i + 1]);
                if (unlikely(!other || other == cpu)) continue;
                uint64_t ow = other->total_weight + (other->current_thread ? other->current_thread->weight : 0);
                if (ow < my_weight && ow < target_weight) {
                    if (cpu_simd_mask(other) == my_mask) { target = other; target_weight = ow; }
                    else if (unlikely(!fallback_target)) { fallback_target = other; }
                }
            }

            if (unlikely(!target)) {
                if (fallback_target) target = fallback_target;
                else return;
            }

            if (unlikely(target_weight + (target_weight >> SCHED_PUSH_GAP_SHIFT) >= my_weight)) return;

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
            /*  hunger 检查已删 (同 StealThread — 死代码) */

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

            /* v3 FIX: 推送成功且目标空闲时立刻 IPI 唤醒. 旧版目标若正
             * hlt, 要等一个 idle 量子的 LAPIC tick 才捡起推送线程.
             * 目标非空闲则不打扰 — 它的下一个 tick (至多一个当前量子)
             * 会捡起; Steal 是兜底的 pull 侧机制. */
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

            if (unlikely(root_t->min_vruntime_subtree > avg_vr)) {
                best = first_runnable(root);
                if (likely(best)) RemoveFromQueue(cpu, best);
                return best;
            }

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
                /* v3 FIX: 早退也要 EOI, 否则 LAPIC ISR 位悬挂,
                 * 阻断后续中断 delivery */
                LAPIC::EOI();
                return;
            }
            uint64_t rflags = irq_save();

            /* Early SMP bring-up window: an AP arms its first LAPIC tick and
               sti inside smp_cpu_init() before Schedule::Install() creates its
               idle_thread and binds current_thread. There is nothing to schedule
               yet, so rearm the timer and return. Falling through to the full
               EEVDF path here dereferences a null idle thread and emits a slow
               serial warning on every tick, which floods the log and stalls the
               mouse interrupt / compositor. Latch current to idle as soon as the
               idle thread exists. */
            if (unlikely(!cpu->idle_thread || !cpu->current_thread)) {
                if (cpu->idle_thread) cpu->current_thread = cpu->idle_thread;
                LAPIC::Oneshot(SCHED_VEC, cpu->base_quantum * cpu->lapic_ticks);
                LAPIC::EOI();
                irq_restore(rflags);
                return;
            }

            /* v3 FIX: preempt_count 检查提前到标志消费之前.
             * 旧代码在函数顶部就清掉 need_resched / 消费 yield 标志,
             * 若随后走 preempt_count>1 早退, 抢占请求被无声吞掉,
             * 只能等下个 tick. 现在标志保持锁存 — CheckPreempt()
             * 在 preempt_count 归零后会重新触发 SCHED_VEC. */
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

            /* ctx/SIMD 保存 — 锁外 */
            if (likely(curr_thread && !curr_is_idle)) {
                curr_thread->fs = rdmsr(FS_BASE);
                curr_thread->ctx = *ctx;
                if (unlikely(curr_thread->fx_area)) {
                    cpu->OverLoadableFuncs.StoreSIMDState(curr_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
                }
            }

            /* 账本结算 — 锁外 */
            dynamic_adjust_quantum(cpu, curr_thread, now, cur_tsc);

            /*  实际运行时长 — 由 last_run_time 结算得出 (而非编程量子),
             * 传给 riprate_update 做采样窗口分母 */
            uint64_t last_slice_ms = 0;

            if (likely(curr_thread && !curr_is_idle)) {
                uint64_t delta = now - curr_thread->last_run_time;
                curr_thread->last_run_time = now;
                if (unlikely(delta == 0)) delta = 1;
                last_slice_ms = delta;
                uint64_t w = likely(curr_thread->weight) ? curr_thread->weight : 1024;
                uint64_t vruntime_total = delta * 1024 + curr_thread->vruntime_rem;
                uint64_t vruntime_delta = vruntime_total / w;
                curr_thread->vruntime_rem = vruntime_total % w;
                curr_thread->vruntime += vruntime_delta;

                uint64_t active_weight = cpu->total_weight + w;
                uint64_t avg_total = delta * 1024 + cpu->avg_vruntime_rem;
                uint64_t avg_delta = avg_total / active_weight;
                cpu->avg_vruntime_rem = avg_total % active_weight;
                cpu->avg_vruntime += avg_delta;
            } else if (unlikely(curr_is_idle)) {
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

            /* ★ RIP 反馈采样 — 锁外.
             *  传实际运行时长 (last_slice_ms), riprate_update 内部
             * 按采样点重臂 RIP 快照 — 修复快路径连续运行时把 N 个
             * 量子的 progress 除以单个量子的窗口错位 */
            riprate_update(cpu, curr_thread, ctx->rip, last_slice_ms, now);

            /* 免锁重入判定 (ZOMBIE 强制慢路径) */
            bool need_lock = true;
            uint32_t curr_state_snap = curr_thread ? curr_thread->state : 0xFFFFFFFF;

            if (likely(curr_thread && !curr_is_idle && !yield_req) &&
                likely(curr_state_snap == THREAD_RUNNING)) {
                /* Lockless fast path ONLY when no other thread is queued.
                   thread_count excludes the running curr, so ==0 means there
                   is truly no competitor. A count of 1 means one waiter is
                   ready: the timer tick MUST take the slow path so EEVDF can
                   preempt. Treating ==1 as lockless starved the sole same-core
                   peer (a non-yielding spinner could never be preempted by the
                   tick, observed as ~14s first-schedule stalls). */
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

            /* ---- 真正的上下文切换 ---- */
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

            /* ★ RIP 窗口起点快照 — v3 语义: riprate_update 每次采样后
             * 会重臂此字段 (它只在调度器内部使用), 与 last_run_time
             * 共同构成逐采样的 窗口 */
            next_thread->dispatch_rip = next_thread->ctx.rip;

            quantum = (likely(next_thread != cpu->idle_thread))
                    ? get_dynamic_quantum(cpu, next_thread)
                    : cpu->base_quantum;

            LAPIC::Oneshot(SCHED_VEC, quantum * cpu->lapic_ticks);
            LAPIC::EOI();
            irq_restore(rflags);
        }
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
            /* v3 FIX: "接近 slice 末尾就不抢" 的判定重写.
             * 旧代码两个 bug:
             *   1) remaining_vr = deadline - vruntime 无防御 — 运行中
             *      vruntime 已越过入队时刻的 deadline 时下溢成 ~2^64;
             *   2) PREEMPT_THRESHOLD (2^20 vruntime 单位 ≈ 17 分钟) 与
             *      remaining (∈ [0, ~15]) 量纲不符, 行为随权重组合随机.
             * 新判定: 剩余虚拟 slice 不足 1/4 (ceil) 就不打断 —
             * 省一次上下文切换, 延迟代价 ≤ 1/4 实际 slice.
             * 注意 vruntime 是全局单位, 该分数对任意权重都对应
             * 实际 slice 的同一比例. */
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
               boot path never assigns it), so their first Switch trips
               "Invalid current_thread pointer: 0". Bind every not-yet-running
               CPU to its own idle thread. The BSP already owns init_thread
               (set by InitCPUThread); a live current must never be clobbered. */
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