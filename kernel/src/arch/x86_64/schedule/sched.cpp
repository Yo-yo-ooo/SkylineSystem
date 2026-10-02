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

#include <arch/x86_64/schedule/sched_internal.h>

#define SCHED_STEAL_BATCH 8
#define ZOMBIE_RECLAIM_THRESHOLD 8
#define ZOMBIE_RECLAIM_BATCH 16
#define SCHED_STEAL_THROTTLE 8
/* 推送门槛: 仅当 my_weight > target × (1 + 1/2^SHIFT) 才推.
 * shift 越小门槛越高 (越保守). */
#define SCHED_PUSH_GAP_SHIFT 2



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
    /* EEVDF 对齐 (Linux place_entity / update_deadline):
     * ① 唤醒放置按睡眠时保存的加权 lag: vruntime = avg - vlag/weight,
     *    再钳制到 [avg-slice, avg+2*slice];
     * ② deadline 保护: vruntime 未越过旧 deadline 时不延长 —— 实体在
     *    用完自己 slice 的虚拟跨度前保持"最紧急"位置, 免被立即抢占。
     * deadline 偏移 = virtual_slice (vruntime 单位): 本内核 slice 已按权重
     * 缩放 (get_dynamic_quantum), 一次满 slice 的 vruntime 消耗恒为
     * base_quantum → 与 Linux `ve + slice/w` 在各自单位下同构。 */
    uint64_t virtual_slice = cpu->base_quantum;
    uint64_t max_lag = virtual_slice;
    uint64_t avg_vr = cpu->avg_vruntime;

    if (unlikely(thread->vlag != 0)) {
        /* Linux place_entity: lag 钳制阈值按调度延迟尺度 (≈4×slice), 远宽于
           单 slice —— 睡眠线程可积累更多 lag 信用, 唤醒后更早被选 */
        uint64_t wake_lo = virtual_slice * 4;
        uint64_t wake_hi = wake_lo * 2;
        int64_t w = (int64_t)(likely(thread->weight) ? thread->weight : 1024);
        /* 四舍五入: 短睡眠的小 lag 不被整除截断丢失 */
        int64_t lag_vr = (thread->vlag >= 0) ? ((thread->vlag + w / 2) / w)
                                             : -((-thread->vlag + w / 2) / w);
        uint64_t target;
        if (lag_vr >= 0) {
            target = ((uint64_t)lag_vr > avg_vr) ? 0 : (avg_vr - (uint64_t)lag_vr);
        } else {
            target = avg_vr + (uint64_t)(-lag_vr);
        }
        uint64_t lo = (avg_vr > wake_lo) ? (avg_vr - wake_lo) : 0;
        uint64_t hi = avg_vr + wake_hi;
        if (target < lo) target = lo;
        if (target > hi) target = hi;
        thread->vruntime = target;
        thread->vruntime_rem = 0;
        thread->vlag = 0;
    } else {
        /* 新线程/未睡眠: 对称钳制 (原逻辑) */
        uint64_t target_vr = (avg_vr > max_lag) ? (avg_vr - max_lag) : 0;
        if (thread->vruntime < target_vr) {
            thread->vruntime = target_vr;
            thread->vruntime_rem = 0;
        } else if (thread->vruntime > avg_vr + max_lag * 2) {
            thread->vruntime = avg_vr + max_lag * 2;
            thread->vruntime_rem = 0;
        }
    }

    if (!(thread->deadline && (int64_t)(thread->vruntime - thread->deadline) < 0)) {
        thread->deadline = thread->vruntime + virtual_slice;
    }
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
                /* 防御: last_run_time==0 表示从未被 dispatch 记账(如启动引导
                   线程), 按运行 0ms 处理, 避免 delta=uptime 污染 vruntime。 */
                uint64_t delta = curr_thread->last_run_time
                               ? now - curr_thread->last_run_time : 0;
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
                /* P2-56: 免锁读 thread_count 用原子加载 (volatile 仅防
                   编译器重排, 跨核需 acquire 语义) */
                if (unlikely(__atomic_load_n(&cpu->thread_count, __ATOMIC_ACQUIRE) == 0)) {
                    need_lock = false;
                }
            } else if (unlikely(curr_is_idle &&
                       __atomic_load_n(&cpu->thread_count, __ATOMIC_ACQUIRE) == 0)) {
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
                    quantum = eligibility_capped_quantum(cpu, next_thread);
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
            next_thread->dispatch_count++;

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
                    ? eligibility_capped_quantum(cpu, next_thread)
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
            if (likely(c && (uint32_t)i != cur_cpu->id)) LAPIC::IPI(c->lapic_id, SCHED_VEC);
        }
        LAPIC::IPI(cur_cpu->lapic_id, SCHED_VEC);
    }
}