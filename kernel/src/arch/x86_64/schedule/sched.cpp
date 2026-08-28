// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// sched.cpp - Core Scheduler Implementation (EEVDF)
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
#define WAIT_THREAD_TIMEOUT_MS 1000ULL
#define PREEMPT_THRESHOLD (1024ULL * 1024ULL)
#define SCHED_HUNGER_THRESHOLD (1024ULL * 1024ULL * 5)



extern art_tree *pid2proc_tree;
extern art_tree *NOT_RUNQ_P;
extern spinlock_t PID2PROC_TREE_LOCK;
extern spinlock_t PROC_LIST_LOCK;
extern uint64_t sched_pid;
extern uint64_t sched_tid;

/* 优化: per-CPU 游标 64B 对齐, 消除 CPU 间伪共享 */
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


static inline uint64_t sced_rdtsc() {
    uint32_t lo, hi;
    asm volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

struct alignas(64) dyn_adjust_ctx {   /* 优化: 64B 对齐消除伪共享 */
    uint64_t last_tsc;
    uint64_t last_ctx_sw;
    uint64_t idle_tsc;
    uint64_t total_tsc;
    uint64_t last_adjust_ms;
};
static dyn_adjust_ctx dyn_ctx[MAX_CPU];

/* 修复: 增加 cur_tsc 参数 —— TSC 读取提到锁外执行,
   使锁内临界区只剩纯算术, 最小化 sched_lock 持有时间 */
static void dynamic_adjust_quantum(cpu_t *cpu, thread_t *curr_thread,
                                   uint64_t now_ms, uint64_t cur_tsc) {
    uint32_t id = cpu->id;

    // 入口处钳位基准值，防止初始异常导致逻辑失效
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
                if (likely(cpu->base_quantum < 15)) cpu->base_quantum++;
            } else if (unlikely(idle_ratio < 10 && ctx_sw > 500)) {
                if (likely(cpu->base_quantum > 2)) cpu->base_quantum--;
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

static inline uint64_t get_dynamic_quantum(cpu_t *cpu, thread_t *thread) {
    if (unlikely(!thread || thread == cpu->idle_thread)) return cpu->base_quantum;
    if (unlikely(thread->custom_quantum > 0)) return thread->custom_quantum;
    uint64_t weight = likely(thread->weight) ? thread->weight : 1024;
    uint64_t quantum = (cpu->base_quantum * weight) / 1024;
    if (unlikely(quantum < 1)) quantum = 1;
    if (unlikely(quantum > cpu->base_quantum * 8)) quantum = cpu->base_quantum * 8;
    return quantum;
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
        asm volatile("sti; hlt; cli" ::: "memory");
    }
}

/* 修复: 拆分为"锁外检查+告警"与"锁内回写"两部分,
   cpu->current_thread 的修正写入移入 sched_lock 临界区 */
static inline thread_t* safe_get_current_thread(cpu_t *cpu, bool &invalid) {
    thread_t *t = cpu->current_thread;
    if (unlikely((uintptr_t)t < 0xFFFF800000000000)) {
        kwarn("Invalid current_thread pointer: %p, resetting to idle\n", t);
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
    uint64_t virtual_slice = cpu->base_quantum;   /* 读方处于持有者 sched_lock 的上下文 */
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

                    uint64_t rflags1;
                    int retries = 0;
                    while (unlikely(!spin_trylock_irqsave(&victim->sched_lock, &rflags1))) {
                        if (unlikely(++retries > 100)) break;
                        asm volatile("pause");
                    }
                    if (unlikely(retries > 100)) continue;

                    if (victim->thread_count <= 1) {
                        spin_unlock_irqrestore(&victim->sched_lock, rflags1);
                        continue;
                    }

                    /* 优化: 锁内先快照 current/idle 指针, 循环中反复读
                       current_thread(无锁写的字段)没有意义, 快照一次即可 */
                    thread_t * const victim_curr  = victim->current_thread;
                    thread_t * const victim_idle  = victim->idle_thread;
                    const uint64_t hunger_limit   = victim->avg_vruntime + SCHED_HUNGER_THRESHOLD;

                    thread_t *stolen_batch[SCHED_STEAL_BATCH];
                    int stolen_count = 0;
                    rb_node_t *node = rb_last(victim->runqueue_root.node);
                    while (likely(node && stolen_count < SCHED_STEAL_BATCH)) {
                        if (unlikely(victim->thread_count <= 1)) break;
                        rb_node_t *prev_node = rb_prev(node);
                        if (likely(prev_node)) PREFETCH_R(prev_node);
                        thread_t *stolen = rb_to_thread(node);
                        /* 跳过受害者当前线程 —— 覆盖"解锁→current_thread
                           迟到提交"窗口, 以及任何唤醒路径错误重插 current 的场景。
                           state 字段无法区分排队/执行中, 指针比较是唯一判据 */
                        if (unlikely(stolen == victim_curr)) { node = prev_node; continue; }
                        /* 防御: idle 线程绝不允许离开所属 CPU
                           (被偷走后受害 CPU 将失去 idle 兜底) */
                        if (unlikely(stolen == victim_idle))  { node = prev_node; continue; }
                        if (unlikely(stolen->state != THREAD_RUNNING)) { node = prev_node; continue; }
                        if (unlikely(stolen->timer_bucket != nullptr)) { node = prev_node; continue; }
                        if (unlikely(stolen->vruntime > hunger_limit)) { node = prev_node; continue; }

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
            uint64_t my_weight = cpu->total_weight + (cpu->current_thread ? cpu->current_thread->weight : 0);
            if (cpu->thread_count < 2) return;

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

            cpu_t *lock_a = (cpu->id < target->id) ? cpu : target;
            cpu_t *lock_b = (cpu->id < target->id) ? target : cpu;

            uint64_t rflags = spin_lock_irqsave(&lock_a->sched_lock);
            uint64_t rflags_b;
            if (unlikely(!spin_trylock_irqsave(&lock_b->sched_lock, &rflags_b))) {
                spin_unlock_irqrestore(&lock_a->sched_lock, rflags);
                return;
            }

            /* TryPush 与 StealThread 扫描结构相同,
               同样存在"旧 current 已入队但 current_thread 未提交"窗口 */
            thread_t * const my_curr = cpu->current_thread;
            thread_t * const my_idle = cpu->idle_thread;
            const uint64_t hunger_limit = cpu->avg_vruntime + SCHED_HUNGER_THRESHOLD;

            int push_count = 0;
            rb_node_t *node = rb_last(cpu->runqueue_root.node);
            while (likely(node && push_count < SCHED_STEAL_BATCH)) {
                uint64_t tc_w = target->total_weight + (target->current_thread ? target->current_thread->weight : 0);
                uint64_t mc_w = cpu->total_weight + (cpu->current_thread ? cpu->current_thread->weight : 0);
                if (tc_w >= mc_w) break;

                rb_node_t *prev_node = rb_prev(node);
                if (likely(prev_node)) PREFETCH_R(prev_node);
                thread_t *to_push = rb_to_thread(node);
                if (unlikely(to_push == my_curr)) { node = prev_node; continue; }   /* 修复#3 */
                if (unlikely(to_push == my_idle)) { node = prev_node; continue; }   /* 防御 */
                if (unlikely(to_push->timer_bucket != nullptr)) { node = prev_node; continue; }
                if (unlikely(to_push->vruntime > hunger_limit)) { node = prev_node; continue; }

                __atomic_store_n(&to_push->state, THREAD_TRANSFER, __ATOMIC_RELEASE);
                RemoveFromQueue(cpu, to_push);
                to_push->cpu_num = target->id;
                to_push->timer_cpu = target->id;
                __atomic_store_n(&to_push->state, THREAD_RUNNING, __ATOMIC_RELEASE);
                InsertToQueue(target, to_push);
                push_count++;
                node = prev_node;
            }

            /* 显式钳位 has_surplus, 不再依赖 RemoveFromQueue 的副作用。
               (原实现下推线程必经 RemoveFromQueue, count==1 时已顺带清标志,
                此行使语义显式化, 防止未来重构破坏该隐式依赖) */
            if (unlikely(cpu->thread_count < 2)) cpu->has_surplus = false;

            cpu->sched_stats.push_success += push_count;
            spin_unlock_irqrestore(&lock_b->sched_lock, rflags_b);
            spin_unlock_irqrestore(&lock_a->sched_lock, rflags);
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
            if (unlikely(!cpu)) return;

            /* 外层: 关中断保护整个处理过程 (若 SCHED_VEC 为陷阱门,
               必须在此处 cli, 否则下方持锁期间本地重入 Switch 会自死锁) */
            uint64_t rflags = irq_save();

            if (unlikely(__atomic_load_n(&need_resched_flags[cpu->id], __ATOMIC_ACQUIRE))) {
                __atomic_store_n(&need_resched_flags[cpu->id], false, __ATOMIC_RELEASE);
            }

            if (unlikely(cpu->preempt_count > 1)) {
                if (likely(cpu->current_thread)) {
                    uint64_t q = get_dynamic_quantum(cpu, cpu->current_thread);
                    LAPIC::Oneshot(SCHED_VEC, q * cpu->lapic_ticks);
                }
                LAPIC::EOI();
                irq_restore(rflags);
                return;
            }

            cpu->tick_count++;
            uint64_t now = PIT::TimeSinceBootMS();
            uint64_t cur_tsc = sced_rdtsc();   /* 修复: TSC 串行化读取提到锁外 */

            bool curr_invalid = false;
            thread_t *curr_thread = safe_get_current_thread(cpu, curr_invalid); /* 告警在锁外打印 */

            const bool curr_is_idle = (curr_thread == cpu->idle_thread);

            /* curr_thread 当前由本 CPU 独占(刚在运行), ctx/SIMD 保存在锁外安全 */
            if (likely(curr_thread && !curr_is_idle)) {
                curr_thread->fs = rdmsr(FS_BASE);
                curr_thread->ctx = *ctx;
                if (unlikely(curr_thread->fx_area)) {
                    cpu->OverLoadableFuncs.StoreSIMDState(curr_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
                }
            }

            /* ============================================================
             * 修复(问题1+2): sched_lock 临界区统一使用 spin_lock_irqsave,
             * 且 dynamic_adjust_quantum / vruntime 结算全部移入锁内.
             * 此前 base_quantum 写、avg_vruntime 更新、total_weight 读
             * 均在锁外, 与远端 TryPush/StealThread 持锁访问同一字段构成数据竞争.
             * ============================================================ */
            uint64_t sflags = spin_lock_irqsave(&cpu->sched_lock);

            if (unlikely(curr_invalid)) {
                cpu->current_thread = curr_thread;   /* 修复: 防御性回写移入锁内 */
            }

            dynamic_adjust_quantum(cpu, curr_thread, now, cur_tsc);   /* 修复: 移入锁内 */

            if (likely(curr_thread && !curr_is_idle)) {
                uint64_t delta = now - curr_thread->last_run_time;
                curr_thread->last_run_time = now;
                if (unlikely(delta == 0)) delta = 1;
                uint64_t w = likely(curr_thread->weight) ? curr_thread->weight : 1024;
                uint64_t vruntime_total = delta * 1024 + curr_thread->vruntime_rem;
                uint64_t vruntime_delta = vruntime_total / w;
                curr_thread->vruntime_rem = vruntime_total % w;
                curr_thread->vruntime += vruntime_delta;

                uint64_t active_weight = cpu->total_weight + w;   /* 修复: 锁内读 total_weight */
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

            thread_t *zombie_to_free = nullptr;
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

            thread_t *next_thread = Pick(cpu);

            spin_unlock_irqrestore(&cpu->sched_lock, sflags);

            reclaim_zombie_list(cpu, zombie_to_free);

            irq_restore(rflags);

            if (unlikely((cpu->tick_count & 0x1F) == 0)) TryPush(cpu);

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
            /* 修复: release 原子写 —— kill_thread_batch 等远端路径
               持锁读取 current_thread, 至少保证存储有序可见 */
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

        /* 注: 此处对 avg_vruntime 为无锁咨询式读取, 仅影响抢占启发式判断 */
        if (woked_thread->vruntime <= cpu->avg_vruntime && woked_thread->deadline < curr->deadline) {
            uint64_t cw = likely(curr->weight) ? curr->weight : 1024;
            uint64_t ww = likely(woked_thread->weight) ? woked_thread->weight : 1024;
            uint64_t w_prod = cw * ww;
            uint64_t remaining_vr = curr->deadline - curr->vruntime;
            if (likely(w_prod != 0 && remaining_vr < PREEMPT_THRESHOLD / w_prod)) {
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
            //cpu->current_thread = idle_t;
            //idle_t->last_run_time = PIT::TimeSinceBootMS();

            dyn_ctx[i].last_adjust_ms = PIT::TimeSinceBootMS();
            dyn_ctx[i].last_ctx_sw = cpu->sched_stats.context_switches;
        }
        atomic_store_8((volatile uint8_t*)&PIT::TickHandle, (uint64_t)(uintptr_t)&PIT::Tick_, 0);
    }

    thread_t* this_thread() { cpu_t* cpu = this_cpu(); return likely(cpu) ? cpu->current_thread : nullptr; }
    proc_t *this_proc() { thread_t* t = this_thread(); return likely(t) ? t->parent : nullptr; }
    void Yield() { LAPIC::StopTimer(); asm volatile("int %0" :: "i"(SCHED_VEC)); }
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