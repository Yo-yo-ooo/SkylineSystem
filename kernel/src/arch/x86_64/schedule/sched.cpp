// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/allin.h>
#include <elf/elf.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>
#include <klib/algorithm/art.h>
#include <atomic/atomic.h>

#ifndef THREAD_TRANSFER
#define THREAD_TRANSFER 4
#endif

static uint64_t sched_tid = 0;
static uint64_t sched_pid = 0;
art_tree *pid2proc_tree = nullptr;
spinlock_t PID2PROC_TREE_LOCK = 0;
spinlock_t PROC_LIST_LOCK = 0;

#define SCHED_STEAL_BATCH 8
#define ZOMBIE_RECLAIM_THRESHOLD 8
#define WAIT_THREAD_TIMEOUT_MS 1000ULL

static uint32_t sched_prio_to_weight[16] = {
    /* 0 */ 8192, /* 1 */ 6553, /* 2 */ 5242, /* 3 */ 4194,
    /* 4 */ 3355, /* 5 */ 2684, /* 6 */ 2147, /* 7 */ 1717,
    /* 8 */ 1374, /* 9 */ 1099, /* 10 */ 879, /* 11 */ 703,
    /* 12 */ 562, /* 13 */ 450, /* 14 */ 360, /* 15 */ 288
};

extern uint64_t elf_load(uint8_t *data, pagemap_t *pagemap,
                  uint64_t *tls_offset = nullptr,
                  uint64_t *tls_memsz = nullptr,
                  uint64_t *tls_filesz = nullptr,
                  uint64_t *tls_align = nullptr);

static_assert(sizeof(sched_prio_to_weight)/sizeof(sched_prio_to_weight[0]) == 16,
              "sched_prio_to_weight must have 16 entries");

static inline bool spin_trylock(spinlock_t *lock) {
    return __sync_bool_compare_and_swap(lock, 0, 1);
}

volatile bool need_resched_flags[MAX_CPU] = {false};

static inline uint64_t get_dynamic_quantum(cpu_t *cpu, thread_t *thread) {
    if (!thread || thread == cpu->idle_thread) return cpu->base_quantum;
    if (thread->custom_quantum > 0) return thread->custom_quantum;
    uint64_t weight = thread->weight ? thread->weight : 1024;
    uint64_t quantum = (cpu->base_quantum * weight) / 1024;
    if (quantum < 1) quantum = 1;
    if (quantum > cpu->base_quantum * 8) quantum = cpu->base_quantum * 8;
    return quantum;
}

// FIX [重复代码抽象]: 抽象出统一的僵尸回收函数
static void reclaim_zombie_list(cpu_t *cpu, thread_t *head) {
    thread_t *z = head;
    while (z) {
        thread_t *next = z->zombie_next;
        // FIX [内存泄漏修复]: KillThread 阶段已完成 detach，此处无需获取全局 PROC_LIST_LOCK
        Schedule::FreeThreadResources(z);
        kfree(z);
        cpu->sched_stats.zombie_reclaims++;
        z = next;
    }
}

void sched_idle() {
    while (true) {
        cpu_t *cpu = this_cpu();
        thread_t *zombie_head = nullptr;

        uint64_t rflags;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&cpu->sched_lock);
        if (cpu->zombie_list) {
            zombie_head = cpu->zombie_list;
            cpu->zombie_list = nullptr;
            cpu->zombie_count = 0;
        }
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        reclaim_zombie_list(cpu, zombie_head);

        asm volatile("sti; hlt; cli" ::: "memory");
    }
}

static cpu_t *get_lw_cpu(cpu_t *ref_cpu = nullptr) {
    cpu_t *lw_cpu = nullptr;
    uint32_t ref_mask = ref_cpu ? cpu_simd_mask(ref_cpu) : 0;
    for (int32_t i = 0; i <= smp_last_cpu; i++) {
        cpu_t *cpu = smp_cpu_list[i];
        if (cpu == nullptr) continue;
        if (ref_cpu && cpu_simd_mask(cpu) != ref_mask) continue;
        if (!lw_cpu) { lw_cpu = cpu; continue; }
        uint64_t current_weight = cpu->total_weight + (cpu->current_thread ? cpu->current_thread->weight : 0);
        uint64_t lowest_weight  = lw_cpu->total_weight + (lw_cpu->current_thread ? lw_cpu->current_thread->weight : 0);
        if (current_weight < lowest_weight) lw_cpu = cpu;
    }
    return lw_cpu ? lw_cpu : (ref_cpu ? ref_cpu : this_cpu());
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
    uint64_t weight = thread->weight ? thread->weight : 1024;
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
    while (node) {
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
        if (t->min_vruntime_subtree == min_vr) break;
        t->min_vruntime_subtree = min_vr;
        node = node->parent;
    }
}

static inline void detach_thread_from_proc(thread_t *thread) {
    if (thread->parent && thread->parent->threads) {
        if (thread->next == thread) {
            thread->parent->threads = nullptr;
        } else {
            if (thread->parent->threads == thread) thread->parent->threads = thread->next;
            thread->prev->next = thread->next;
            thread->next->prev = thread->prev;
        }
    }
    thread->parent = nullptr;
    thread->next = thread->prev = nullptr;
}

namespace Schedule {
    namespace Internal {
        void RemoveFromQueue(cpu_t *cpu, thread_t *thread) {
            if (!thread->on_rq) return;
            rb_node_t *node = &thread->rb_node;
            
            rb_node_t *successor = nullptr;
            rb_node_t *update_1 = node->parent;
            rb_node_t *update_2 = nullptr;

            if (node->left && node->right) {
                successor = node->right;
                while (successor->left) successor = successor->left;
                update_1 = successor->parent;
                update_2 = successor;
            }

            rb_erase(&cpu->runqueue_root, node);
            thread->on_rq = false;
            cpu->thread_count--;
            cpu->total_weight -= thread->weight;
            
            if (update_1) update_min_vruntime_upward(update_1);
            if (update_2) update_min_vruntime_upward(update_2);
            
            if (cpu->thread_count == 1) cpu->has_surplus = false;
            if (cpu->thread_count == 0) cpu->has_runnable_thread = false;
        }

        void InsertToQueue(cpu_t *cpu, thread_t *thread) {
            if (thread->on_rq) return;
            calibrate_and_set_deadline(thread, cpu);
            rb_insert(&cpu->runqueue_root, &thread->rb_node, thread_rb_cmp);
            thread->on_rq = true;
            cpu->thread_count++;
            cpu->total_weight += thread->weight;
            if (cpu->thread_count == 1) cpu->has_runnable_thread = true;
            if (cpu->thread_count == 2) cpu->has_surplus = true;
            update_min_vruntime_upward(&thread->rb_node);
        }

        void ProcessAddThread(proc_t *parent, thread_t *thread) {
            uint64_t rflags;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&PROC_LIST_LOCK);
            if (!parent->threads) {
                parent->threads = thread;
                thread->next = thread;
                thread->prev = thread;
            } else {
                thread->next = parent->threads;
                thread->prev = parent->threads->prev;
                parent->threads->prev->next = thread;
                parent->threads->prev = thread;
            }
            spinlock_unlock(&PROC_LIST_LOCK);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        }

        thread_t *StealThread(cpu_t *cpu) {
            uint32_t my_mask = cpu_simd_mask(cpu);
            uint32_t start_cpu = (sched_pid + PIT::TimeSinceBootMS()) % (smp_last_cpu + 1);

            for (int pass = 0; pass < 2; pass++) {
                for (uint32_t k = 0; k <= smp_last_cpu; k++) {
                    uint32_t i = (start_cpu + k) % (smp_last_cpu + 1);
                    cpu_t *victim = smp_cpu_list[i];
                    if (!victim || victim == cpu) continue;
                    if (pass == 0 && cpu_simd_mask(victim) != my_mask) continue;
                    if (!atomic_load_1(&victim->has_surplus, ATOMIC_RELAXED)) continue;

                    cpu->sched_stats.steal_attempts++;

                    uint64_t rflags1;
                    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags1) :: "memory");
                    int retries = 0;
                    while (!spin_trylock(&victim->sched_lock)) {
                        if (++retries > 100) break;
                        asm volatile("pause");
                    }
                    if (retries > 100) {
                        asm volatile("push %0\n\tpopfq" :: "r"(rflags1) : "memory");
                        continue;
                    }

                    if (victim->thread_count <= 1) {
                        spinlock_unlock(&victim->sched_lock);
                        asm volatile("push %0\n\tpopfq" :: "r"(rflags1) : "memory");
                        continue;
                    }

                    thread_t *stolen_batch[SCHED_STEAL_BATCH];
                    int stolen_count = 0;
                    rb_node_t *node = rb_last(victim->runqueue_root.node);
                    while (node && stolen_count < SCHED_STEAL_BATCH) {
                        if (victim->thread_count <= 1) break;
                        thread_t *stolen = rb_to_thread(node);
                        rb_node_t *prev_node = rb_prev(node);
                        if (stolen->state == THREAD_ZOMBIE) { node = prev_node; continue; }
                        if (stolen->timer_bucket != nullptr) {
                            if (stolen->timer_cpu == victim->id) {
                                TimerRemove(stolen);
                                stolen->timer_bucket = nullptr;
                            } else { node = prev_node; continue; }
                        }
                        RemoveFromQueue(victim, stolen);
                        stolen_batch[stolen_count++] = stolen;
                        node = prev_node;
                    }

                    if (stolen_count > 0) {
                        for (int j = 0; j < stolen_count; j++) {
                            __atomic_store_n(&stolen_batch[j]->state, THREAD_TRANSFER, __ATOMIC_RELEASE);
                        }
                        spinlock_unlock(&victim->sched_lock);
                        asm volatile("push %0\n\tpopfq" :: "r"(rflags1) : "memory");

                        uint64_t rflags2;
                        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags2) :: "memory");
                        spinlock_lock(&cpu->sched_lock);
                        for (int j = 0; j < stolen_count; j++) {
                            stolen_batch[j]->cpu_num = cpu->id;
                            stolen_batch[j]->timer_cpu = cpu->id;
                            __atomic_store_n(&stolen_batch[j]->state, THREAD_RUNNING, __ATOMIC_RELEASE);
                            InsertToQueue(cpu, stolen_batch[j]);
                        }
                        thread_t *best = Pick(cpu);
                        spinlock_unlock(&cpu->sched_lock);
                        asm volatile("push %0\n\tpopfq" :: "r"(rflags2) : "memory");

                        if (best) {
                            cpu->sched_stats.thread_steals += stolen_count;
                            return best;
                        }
                        return nullptr;
                    }
                    spinlock_unlock(&victim->sched_lock);
                    asm volatile("push %0\n\tpopfq" :: "r"(rflags1) : "memory");
                }
            }
            return nullptr;
        }

        void TryPush(cpu_t *cpu) {
            cpu->sched_stats.try_pushes++;
            if (!atomic_load_1(&cpu->has_surplus, ATOMIC_RELAXED)) return;
            uint64_t my_weight = cpu->total_weight + (cpu->current_thread ? cpu->current_thread->weight : 0);
            if (cpu->thread_count < 2) return;

            uint32_t my_mask = cpu_simd_mask(cpu);
            cpu_t *target = nullptr;
            cpu_t *fallback_target = nullptr;
            uint64_t target_weight = UINT64_MAX;

            for (int32_t i = 0; i <= smp_last_cpu; i++) {
                cpu_t *other = smp_cpu_list[i];
                if (!other || other == cpu) continue;
                uint64_t ow = other->total_weight + (other->current_thread ? other->current_thread->weight : 0);
                if (ow < my_weight && ow < target_weight) {
                    if (cpu_simd_mask(other) == my_mask) { target = other; target_weight = ow; }
                    else if (!fallback_target) { fallback_target = other; }
                }
            }

            if (!target) {
                if (fallback_target) target = fallback_target;
                else return;
            }

            cpu_t *lock_a = (cpu->id < target->id) ? cpu : target;
            cpu_t *lock_b = (cpu->id < target->id) ? target : cpu;

            uint64_t rflags;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&lock_a->sched_lock);
            if (!spin_trylock(&lock_b->sched_lock)) {
                spinlock_unlock(&lock_a->sched_lock);
                asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
                return;
            }

            int push_count = 0;
            rb_node_t *node = rb_last(cpu->runqueue_root.node);
            while (node && push_count < SCHED_STEAL_BATCH) {
                uint64_t tc_w = target->total_weight + (target->current_thread ? target->current_thread->weight : 0);
                uint64_t mc_w = cpu->total_weight + (cpu->current_thread ? cpu->current_thread->weight : 0);
                if (tc_w >= mc_w) break;

                thread_t *to_push = rb_to_thread(node);
                rb_node_t *prev_node = rb_prev(node);
                if (to_push->timer_bucket != nullptr) {
                    if (to_push->timer_cpu == cpu->id) { TimerRemove(to_push); to_push->timer_bucket = nullptr; } 
                    else { node = prev_node; continue; }
                }

                __atomic_store_n(&to_push->state, THREAD_TRANSFER, __ATOMIC_RELEASE);
                RemoveFromQueue(cpu, to_push);
                to_push->cpu_num = target->id;
                to_push->timer_cpu = target->id;
                __atomic_store_n(&to_push->state, THREAD_RUNNING, __ATOMIC_RELEASE);
                InsertToQueue(target, to_push);
                push_count++;
                node = prev_node;
            }

            cpu->sched_stats.push_success += push_count;
            spinlock_unlock(&lock_b->sched_lock);
            spinlock_unlock(&lock_a->sched_lock);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        }

        thread_t *Pick(cpu_t *cpu) {
            rb_node_t *root = cpu->runqueue_root.node;
            if (!root) return nullptr;

            thread_t *root_t = rb_to_thread(root);
            if (root_t->min_vruntime_subtree > cpu->avg_vruntime) {
                thread_t *best = rb_to_thread(rb_first(root));
                if (best) { RemoveFromQueue(cpu, best); return best; }
                return nullptr;
            }

            rb_node_t *node = root;
            thread_t *best = nullptr;
            while (node) {
                thread_t *t = rb_to_thread(node);
                if (t->vruntime <= cpu->avg_vruntime) { best = t; node = node->left; } 
                else {
                    if (node->left) {
                        thread_t *lt = rb_to_thread(node->left);
                        if (lt->min_vruntime_subtree <= cpu->avg_vruntime) { node = node->left; continue; }
                    }
                    node = node->right;
                }
            }

            if (!best) best = rb_to_thread(rb_first(cpu->runqueue_root.node));
            if (best) RemoveFromQueue(cpu, best);
            return best;
        }

        void Switch(context_t *ctx) {
            LAPIC::StopTimer();
            cpu_t *cpu = this_cpu();
            if (!cpu) return;
            uint64_t rflags;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");

            if (cpu->preempt_count > 1) {
                if (cpu->current_thread) {
                    uint64_t q = get_dynamic_quantum(cpu, cpu->current_thread);
                    LAPIC::Oneshot(SCHED_VEC, q * cpu->lapic_ticks);
                }
                LAPIC::EOI();
                asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
                return;
            }

            cpu->tick_count++;
            thread_t *curr_thread = cpu->current_thread;
            uint64_t now = PIT::TimeSinceBootMS();

            if (curr_thread && curr_thread != cpu->idle_thread) {
                uint64_t delta = now - curr_thread->last_run_time;
                curr_thread->last_run_time = now;
                if (delta == 0) delta = 1;
                uint64_t vruntime_total = delta * 1024 + curr_thread->vruntime_rem;
                uint64_t vruntime_delta = vruntime_total / curr_thread->weight;
                curr_thread->vruntime_rem = vruntime_total % curr_thread->weight;
                curr_thread->vruntime += vruntime_delta;

                uint64_t active_weight = cpu->total_weight + curr_thread->weight;
                if (active_weight > 0) {
                    uint64_t avg_total = delta * 1024 + cpu->avg_vruntime_rem;
                    uint64_t avg_delta = avg_total / active_weight;
                    cpu->avg_vruntime_rem = avg_total % active_weight;
                    cpu->avg_vruntime += avg_delta;
                }
            }

            if (curr_thread && curr_thread != cpu->idle_thread) {
                curr_thread->fs = rdmsr(FS_BASE);
                curr_thread->ctx = *ctx;
                if (curr_thread->fx_area) {
                    cpu->OverLoadableFuncs.StoreSIMDState(curr_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
                }
            }

            spinlock_lock(&cpu->sched_lock);
            uint32_t curr_state = curr_thread ? curr_thread->state : 0xFFFFFFFF;

            if (curr_thread && curr_state == THREAD_ZOMBIE && curr_thread != cpu->idle_thread) {
                cpu->current_thread = nullptr;
                curr_thread->zombie_next = cpu->zombie_list;
                cpu->zombie_list = curr_thread;
                cpu->zombie_count++;
                curr_thread = nullptr;
            } else if (curr_thread && curr_state == THREAD_RUNNING && curr_thread != cpu->idle_thread) {
                InsertToQueue(cpu, curr_thread);
            }

            thread_t *next_thread = Pick(cpu);

            // FIX [锁冗余 & 满载兜底]: 合并到 sched_lock 临界区内摘取僵尸，锁外直接释放
            thread_t *zombie_to_free = nullptr;
            thread_t *zombie_tail = nullptr;
            if (cpu->zombie_count >= ZOMBIE_RECLAIM_THRESHOLD) {
                int moved = 0;
                thread_t *z = cpu->zombie_list;
                while (z && moved < 4) {
                    thread_t *next = z->zombie_next;
                    z->zombie_next = zombie_to_free;
                    zombie_to_free = z;
                    if (!zombie_tail) zombie_tail = z;
                    z = next;
                    moved++;
                }
                if (zombie_to_free) {
                    cpu->zombie_list = z;
                    cpu->zombie_count -= moved;
                }
            }
            spinlock_unlock(&cpu->sched_lock);

            // 锁外同步释放资源，作为极端满载场景下的兜底机制
            reclaim_zombie_list(cpu, zombie_to_free);

            if (!next_thread) {
                next_thread = StealThread(cpu);
                if (!next_thread) next_thread = cpu->idle_thread;
            }

            if (!next_thread) {
                LAPIC::EOI();
                asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
                return;
            }

            uint64_t quantum = cpu->base_quantum;
            bool is_switch = (next_thread != curr_thread);
            if (is_switch) {
                cpu->current_thread = next_thread;
                cpu->sched_stats.context_switches++;
                if (next_thread != cpu->idle_thread) next_thread->last_run_time = now;
            }

            if ((cpu->tick_count & 0x1F) == 0) TryPush(cpu);

            if (!is_switch) {
                if (next_thread != cpu->idle_thread) {
                    quantum = get_dynamic_quantum(cpu, next_thread);
                    LAPIC::Oneshot(SCHED_VEC, quantum * cpu->lapic_ticks);
                }
                LAPIC::EOI();
                asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
                return;
            }

            *ctx = next_thread->ctx;
            TSS::SetRSP(cpu->id, 0, (void*)next_thread->kernel_rsp);
            cpu->kernel_stack = next_thread->kernel_rsp;

            if (!curr_thread || curr_thread->pagemap != next_thread->pagemap) {
                VMM::SwitchPageMap(next_thread->pagemap);
                asm volatile("cli");
            }

            cpu->OverLoadableFuncs.WRFSBASE(next_thread->fs);
            if (next_thread->fx_area) {
                cpu->OverLoadableFuncs.LoadSIMDState(next_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
            }

            if (next_thread != cpu->idle_thread) quantum = get_dynamic_quantum(cpu, next_thread);
            else quantum = cpu->base_quantum;
            
            LAPIC::Oneshot(SCHED_VEC, quantum * cpu->lapic_ticks);
            LAPIC::EOI();
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        }

        void Preempt(context_t *ctx) { Switch(ctx); }
    }

    void KillThread(thread_t *thread) {
        if (!thread) return;
        uint64_t wait_start = PIT::TimeSinceBootMS();
        while (__atomic_load_n(&thread->state, __ATOMIC_ACQUIRE) == THREAD_TRANSFER) {
            if (PIT::TimeSinceBootMS() - wait_start > WAIT_THREAD_TIMEOUT_MS) break;
            asm volatile("pause");
        }

        cpu_t *cpu = get_cpu(thread->cpu_num);
        if (!cpu) return;
        bool was_running = false;

        uint64_t rflags;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&PROC_LIST_LOCK);
        detach_thread_from_proc(thread);
        spinlock_unlock(&PROC_LIST_LOCK);

        spinlock_lock(&cpu->sched_lock);
        thread->state = THREAD_ZOMBIE;
        was_running = (cpu->current_thread == thread);
        if (thread->on_rq) {
            Internal::RemoveFromQueue(cpu, thread);
            thread->zombie_next = cpu->zombie_list;
            cpu->zombie_list = thread;
            cpu->zombie_count++;
        } else if (thread->timer_bucket != nullptr) {
            Internal::TimerRemove(thread);
            thread->zombie_next = cpu->zombie_list;
            cpu->zombie_list = thread;
            cpu->zombie_count++;
        }
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        if (was_running) {
            cpu_t *cur_cpu = this_cpu();
            if (cpu != cur_cpu) LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
        }
    }

    void KillProcessThreads(proc_t *proc) {
        if (!proc) return;
        while (true) {
            thread_t *batch[64];
            int count = 0;
            uint64_t rflags;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&PROC_LIST_LOCK);
            thread_t *t = proc->threads;
            if (t) {
                thread_t *start = t;
                do {
                    if (count < 64) { batch[count++] = t; t = t->next; } else break;
                } while (t != start);
            }
            spinlock_unlock(&PROC_LIST_LOCK);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

            if (count == 0) break;
            for (int i = 0; i < count; i++) KillThread(batch[i]);
        }
    }

    void WaitForThreadOffCpu(thread_t *thread) {
        if (!thread) return;
        uint64_t start_time = PIT::TimeSinceBootMS();
        while (true) {
            uint32_t cpu_num = __atomic_load_n(&thread->cpu_num, __ATOMIC_ACQUIRE);
            if (cpu_num >= MAX_CPU) break;
            cpu_t *cpu = smp_cpu_list[cpu_num];
            if (!cpu) break;
            thread_t *curr = __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE);
            if (curr != thread) break;
            if (PIT::TimeSinceBootMS() - start_time > WAIT_THREAD_TIMEOUT_MS) Panic("WaitForThreadOffCpu: Thread stuck on CPU (timeout)");
            asm volatile("pause");
        }
    }

    void TriggerPreempt(thread_t *woked_thread) {
        if (!woked_thread) return;
        uint32_t cpu_num = __atomic_load_n(&woked_thread->cpu_num, __ATOMIC_ACQUIRE);
        if (cpu_num >= MAX_CPU) return;
        cpu_t *cpu = smp_cpu_list[cpu_num];
        if (!cpu) return;

        thread_t *curr = __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE);
        if (!curr || curr == cpu->idle_thread) {
            LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
            return;
        }

        if (woked_thread->vruntime <= cpu->avg_vruntime && woked_thread->deadline < curr->deadline) {
            uint64_t remaining_vr = curr->deadline - curr->vruntime;
            if (remaining_vr * curr->weight * woked_thread->weight < 1024 * 1024) {
                return; 
            }

            cpu_t *cur_cpu = this_cpu();
            if (cpu != cur_cpu) {
                LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
            } else {
                __atomic_store_n(&need_resched_flags[cpu->id], true, __ATOMIC_RELEASE);
            }
        }
    }

    void Init() {
        if (!pid2proc_tree) {
            pid2proc_tree = (art_tree*)kmalloc(sizeof(art_tree));
            if (art_tree_init(pid2proc_tree) != 0) Panic("ART TREE INIT FAILED!");
        }
        idt_install_irq(SCHED_VEC, (void*)Schedule::Internal::Preempt);
        idt_install_irq(SCHED_VEC + 1, (void*)Schedule::Internal::Switch);
        idt_set_ist(SCHED_VEC, 0);
        idt_set_ist(SCHED_VEC + 1, 0);
    }

    void Install() {
        for (uint32_t i = 0; i <= smp_last_cpu; i++) {
            cpu_t *cpu = smp_cpu_list[i];
            if (!cpu) continue;
            cpu->timer_last_tick = PIT::TimeSinceBootMS();
            proc_t *proc = Schedule::NewProcess(false);
            thread_t *idle_t = Schedule::NewKernelThread(proc, cpu->id, 15, sched_idle);
            uint64_t rflags;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&cpu->sched_lock);
            Internal::RemoveFromQueue(cpu, idle_t);
            spinlock_unlock(&cpu->sched_lock);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
            cpu->idle_thread = idle_t;
        }
        atomic_store_8((volatile uint8_t*)&PIT::TickHandle, (uint64_t)(uintptr_t)&PIT::Tick_, 0);
    }

    proc_t *NewProcess(bool user) {
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        if (!proc) return nullptr;
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid,1,ATOMIC_RELAXED);
        proc->pagemap = (user ? VMM::NewPM() : kernel_pagemap);
        if (user && !proc->pagemap) { kfree(proc); return nullptr; }
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        if (!proc->FDMan) { if (user) VMM::DestroyPM(proc->pagemap); kfree(proc); return nullptr; }
        fd_manager_init(proc->FDMan);
        proc->fd_count = 4;
        uint64_t rflags;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_insert(pid2proc_tree,(const uint8_t*)&proc->id,8,proc);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        return proc;
    }

    void PrepareUserStack(thread_t *thread, int32_t argc, char *argv[], char *envp[]) {
        if (argc <= 0 || !argv || !envp) return;
        char **kernel_argv = nullptr, **kernel_envp = nullptr;
        uint64_t *thread_argv = nullptr, *thread_envp = nullptr;
        int32_t envc = 0; uint64_t offset = 0;
        uint64_t stack_top = 0; pagemap_t *restore = nullptr;

        kernel_argv = (char**)kmalloc(argc * sizeof(char*));
        if (!kernel_argv) return;
        for (int32_t i = 0; i < argc; i++) kernel_argv[i] = nullptr;
        for (int32_t i = 0; i < argc; i++) {
            if(!argv[i]) goto cleanup;
            int32_t size = strlen(argv[i]) + 1;
            kernel_argv[i] = (char*)kmalloc(size);
            if (!kernel_argv[i]) goto cleanup;
            __memcpy(kernel_argv[i], argv[i], size);
        }

        while (envp[envc++]); envc -= 1;
        if (envc > 0) {
            kernel_envp = (char**)kmalloc(envc * sizeof(char*));
            if (!kernel_envp) goto cleanup;
            for (int32_t i = 0; i < envc; i++) kernel_envp[i] = nullptr;
            for (int32_t i = 0; i < envc; i++) {
                if(!envp[i]) goto cleanup;
                int32_t size = strlen(envp[i]) + 1;
                kernel_envp[i] = (char*)kmalloc(size);
                if (!kernel_envp[i]) goto cleanup;
                __memcpy(kernel_envp[i], envp[i], size);
            }
        }

        thread_argv = (uint64_t*)kmalloc(argc * sizeof(uint64_t));
        if (!thread_argv) goto cleanup;
        stack_top = thread->ctx.rsp;
        if ((argc + envc) % 2 == 0) offset = 8;
        restore = VMM::SwitchPageMap(thread->pagemap);
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(kernel_argv[i]) + 1;
            offset += ALIGN_UP(size, 16);
            thread_argv[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_argv[i], size);
        }

        if (envc > 0) {
            thread_envp = (uint64_t*)kmalloc(envc * sizeof(uint64_t));
            if(!thread_envp) { VMM::SwitchPageMap(restore); goto cleanup; }
            for (int32_t i = 0; i < envc; i++) {
                int32_t size = strlen(kernel_envp[i]) + 1;
                offset += ALIGN_UP(size, 16);
                thread_envp[i] = stack_top - offset;
                __memcpy((void*)(stack_top - offset), kernel_envp[i], size);
            }
        }

        offset += 8; *(uint64_t*)(stack_top - offset) = 0;
        for (int32_t i = envc - 1; i >= 0; i--) { offset += 8; *(uint64_t*)(stack_top - offset) = thread_envp[i]; }
        offset += 8; *(uint64_t*)(stack_top - offset) = 0;
        for (int32_t i = argc - 1; i >= 0; i--) { offset += 8; *(uint64_t*)(stack_top - offset) = thread_argv[i]; }
        offset += 8; *(uint64_t*)(stack_top - offset) = argc;
        VMM::SwitchPageMap(restore);
        thread->ctx.rsp = stack_top - offset;

    cleanup:
        if (kernel_argv) { for (int32_t i = 0; i < argc; i++) if (kernel_argv[i]) kfree(kernel_argv[i]); kfree(kernel_argv); }
        if (kernel_envp) { for (int32_t i = 0; i < envc; i++) if (kernel_envp[i]) kfree(kernel_envp[i]); kfree(kernel_envp); }
        if (thread_argv) kfree(thread_argv);
        if (thread_envp) kfree(thread_envp);
    }

    thread_t *NewKernelThread(proc_t *parent, uint32_t cpu_num, int32_t priority, void *entry) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        thread->timer_cpu = cpu_num;
        thread->id = atomic_add_fetch_8(&sched_tid,1,ATOMIC_RELAXED);
        thread->cpu_num = cpu_num; thread->parent = parent;
        thread->pagemap = parent->pagemap;
        thread->priority = priority > 15 ? 15 : priority;
        thread->weight = sched_prio_to_weight[thread->priority];
        cpu_t *cpu = get_cpu(cpu_num);
        uint64_t base_vruntime = cpu->avg_vruntime;
        uint64_t half_slice = cpu->base_quantum / 2;
        thread->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((cpu->XsaveSize), PAGE_SIZE), true);
        if (!thread->fx_area) { kfree(thread); return nullptr; }
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, thread->fx_area); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
        thread->kernel_stack = kernel_stack; thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);
        thread->stack = kernel_stack; thread->ctx.rip = (uint64_t)entry;
        thread->ctx.cs = 0x08; thread->ctx.ss = 0x10; thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread->kernel_rsp; thread->thread_stack = thread->ctx.rsp;
        thread->state = THREAD_RUNNING;
        Schedule::Internal::ProcessAddThread(parent, thread);
        
        uint64_t rflags;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        return thread;
    }

    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int32_t priority, const char *Path, int32_t argc, char *argv[], char *envp[]) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        thread->timer_cpu = cpu_num;
        thread->id = atomic_add_fetch_8(&sched_tid,1,ATOMIC_RELAXED);
        thread->cpu_num = cpu_num; thread->parent = parent; thread->pagemap = parent->pagemap;
        thread->priority = priority > 15 ? 15 : priority;
        thread->weight = sched_prio_to_weight[thread->priority];
        cpu_t *cpu = get_cpu(cpu_num);
        uint64_t base_vruntime = cpu->avg_vruntime;
        uint64_t half_slice = cpu->base_quantum / 2;
        thread->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;

        __hmap_s_mp *MP = GetMount(Path);
        if(!MP) { kerrorln("Cannot Find Mount Point!!!"); kfree(thread); return nullptr; }
        void *FileDesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
        if (!FileDesc) { kfree(thread); return nullptr; }
        _memset(FileDesc, 0, MP->FSOPS->SIZEOF_FILE_DESC);
        if(MP->FSOPS->open(FileDesc, Path, O_RDONLY) != 0) { kfree(FileDesc); kfree(thread); return nullptr; }
        uint64_t FSize = MP->FSOPS->fsize(FileDesc);
        uint8_t *buffer = (uint8_t*)kmalloc(FSize);
        if (!buffer) { MP->FSOPS->close(FileDesc); kfree(FileDesc); kfree(thread); return nullptr; }
        MP->FSOPS->read(FileDesc, buffer, FSize, 0);
        MP->FSOPS->close(FileDesc); kfree(FileDesc);

        uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
        _memset(&thread->ctx, 0, sizeof(context_t));
        thread->ctx.rip = elf_load(buffer, thread->pagemap, &tls_offset, &tls_memsz, &tls_filesz, &tls_align);
        if (thread->ctx.rip == 0) { kerrorln("ELF load failed!"); kfree(buffer); kfree(thread); return nullptr; }

        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        if (!thread->fx_area) { kfree(buffer); kfree(thread); return nullptr; }
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, thread->fx_area); kfree(buffer); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
        thread->kernel_stack = kernel_stack; thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);
        uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
        if (!thread_stack) { VMM::Free(kernel_pagemap, thread->fx_area); VMM::Free(kernel_pagemap, (void*)kernel_stack); kfree(buffer); kfree(thread); return nullptr; }
        thread->stack = thread_stack; thread->thread_stack = thread_stack + 8 * PAGE_SIZE;
        uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
        if (!sig_stack) { VMM::Free(kernel_pagemap, thread->fx_area); VMM::Free(kernel_pagemap, (void*)kernel_stack); VMM::Free(thread->pagemap, (void*)thread_stack); kfree(buffer); kfree(thread); return nullptr; }
        thread->sig_stack = sig_stack;
        thread->ctx.cs = 0x23; thread->ctx.ss = 0x1b; thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread->thread_stack;
        PrepareUserStack(thread, argc, argv, envp);
        thread->thread_stack = thread->ctx.rsp;

        if (tls_memsz > 0) {
            if (tls_align == 0) tls_align = 16;
            uint64_t total_tls_size = ALIGN_UP(tls_memsz, tls_align) + 8;
            uint64_t tls_pages = DIV_ROUND_UP(total_tls_size, PAGE_SIZE);
            uint64_t tls_mem = (uint64_t)VMM::Alloc(thread->pagemap, tls_pages, true);
            if (!tls_mem) { kfree(buffer); kfree(thread); return nullptr; }
            uint64_t tcb_base = tls_mem + ALIGN_UP(tls_memsz, tls_align);
            VMM::SwitchPageMap(thread->pagemap);
            __memcpy((void*)(tcb_base - ALIGN_UP(tls_memsz, tls_align)), (void*)(buffer + tls_offset), tls_filesz);
            *(uint64_t*)tcb_base = tcb_base;
            VMM::SwitchPageMap(kernel_pagemap);
            thread->fs = tcb_base; thread->tls_base = tls_mem; thread->tls_pages = tls_pages;
        }

        kfree(buffer);
        thread->state = THREAD_RUNNING;
        Schedule::Internal::ProcessAddThread(parent, thread);
        
        uint64_t rflags;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        return thread;
    }

    thread_t *ForkThread(proc_t *proc, thread_t *parent, void *frame) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        cpu_t *parent_cpu = get_cpu(parent->cpu_num);
        cpu_t *cpu = get_lw_cpu(parent_cpu);
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        if (!thread->fx_area) { kfree(thread); return nullptr; }

        uint64_t rflags;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&parent_cpu->sched_lock);
        if (parent_cpu->current_thread == parent && parent->fx_area) {
            parent_cpu->OverLoadableFuncs.StoreSIMDState(parent->fx_area, parent_cpu->XsaveMaskLo, parent_cpu->XsaveMaskHi);
        }
        __memcpy(thread->fx_area, parent->fx_area, cpu->XsaveSize);
        spinlock_unlock(&parent_cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, thread->fx_area); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);

        thread->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
        thread->cpu_num = cpu->id; thread->parent = proc; thread->IsForkThread = true; thread->pagemap = proc->pagemap;
        thread->kernel_stack = kernel_stack; thread->kernel_rsp = kernel_stack + 4 * PAGE_SIZE;
        thread->stack = parent->stack; thread->sig_stack = parent->sig_stack;
        thread->tls_base = parent->tls_base; thread->tls_pages = parent->tls_pages;
        thread->timer_cpu = cpu->id;
        Schedule::Internal::ProcessAddThread(proc, thread);
        __memcpy(&thread->ctx, frame, sizeof(context_t));
        thread->ctx.rsp = ((context_t*)frame)->rsp;
        thread->ctx.cs = 0x23; thread->ctx.ss = 0x1b; thread->ctx.rflags = ((syscall_frame_t*)frame)->r11;
        thread->ctx.rax = 0; thread->ctx.rip = ((syscall_frame_t*)frame)->rcx;
        thread->thread_stack = thread->ctx.rsp; thread->fs = rdmsr(FS_BASE); thread->state = THREAD_RUNNING;
        thread->priority = parent->priority; thread->weight = parent->weight;
        
        uint64_t base_vruntime = cpu->avg_vruntime;
        uint64_t half_slice = cpu->base_quantum / 2;
        thread->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;

        uint64_t rflags2;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags2) :: "memory");
        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags2) : "memory");
        return thread;
    }

    proc_t *ForkProcess() {
        proc_t *parent = this_proc();
        if (!parent) return nullptr;
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        if (!proc) return nullptr;
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid,1,ATOMIC_RELAXED); proc->parent = parent;
        proc->pagemap = VMM::Fork(parent->pagemap);
        if (!proc->pagemap) { kfree(proc); return nullptr; }
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        if (!proc->FDMan) { VMM::DestroyPM(proc->pagemap); kfree(proc); return nullptr; }

        uint64_t rflags;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&PROC_LIST_LOCK);
        __memcpy(proc->FDMan, parent->FDMan, sizeof(fd_manager_t));
        proc->fd_count = parent->fd_count;
        if (!parent->children) parent->children = proc;
        else {
            proc_t *last = parent->children;
            while (last->sibling) last = last->sibling;
            last->sibling = proc;
        }
        spinlock_unlock(&PROC_LIST_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_insert(pid2proc_tree, (const uint8_t*)&proc->id, 8, proc);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        return proc;
    }

    thread_t* this_thread() { cpu_t* cpu = this_cpu(); return cpu ? cpu->current_thread : nullptr; }
    proc_t *this_proc() { thread_t* t = this_thread(); return t ? t->parent : nullptr; }
    void Yield() { LAPIC::StopTimer(); asm volatile("int %0" :: "i"(SCHED_VEC + 1)); }
    void PAUSE() { LAPIC::StopTimer(); }
    void Resume() {
        cpu_t* cur_cpu = this_cpu();
        if (!cur_cpu) return;
        for (int32_t i = 0; i <= smp_last_cpu; i++) {
            if (smp_cpu_list[i] && i != cur_cpu->id) LAPIC::IPI(smp_cpu_list[i]->lapic_id, SCHED_VEC + 1);
        }
        LAPIC::IPI(cur_cpu->lapic_id, SCHED_VEC + 1);
    }
}