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

#ifndef THREAD_TRANSFER
#define THREAD_TRANSFER 4
#endif

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

static uint32_t per_cpu_steal_cursor[MAX_CPU] = {0};

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

struct dyn_adjust_ctx {
    uint64_t last_tsc;
    uint64_t last_ctx_sw;
    uint64_t idle_tsc;
    uint64_t total_tsc;
    uint64_t last_adjust_ms;
};
static dyn_adjust_ctx dyn_ctx[MAX_CPU] = {0};

static void dynamic_adjust_quantum(cpu_t *cpu, thread_t *curr_thread, uint64_t now_ms) {
    uint32_t id = cpu->id;
    
    // 修复：入口处钳位基准值，防止初始异常导致逻辑失效
    if (cpu->base_quantum < 2) cpu->base_quantum = 2;
    if (cpu->base_quantum > 15) cpu->base_quantum = 15;

    uint64_t cur_tsc = sced_rdtsc();
    
    if (dyn_ctx[id].last_tsc != 0) {
        uint64_t elapsed = cur_tsc - dyn_ctx[id].last_tsc;
        if (curr_thread == cpu->idle_thread) {
            dyn_ctx[id].idle_tsc += elapsed;
        }
        dyn_ctx[id].total_tsc += elapsed;
    }
    dyn_ctx[id].last_tsc = cur_tsc;
    
    if (now_ms - dyn_ctx[id].last_adjust_ms > 100) {
        if (dyn_ctx[id].total_tsc > 0) {
            uint64_t idle_ratio = (dyn_ctx[id].idle_tsc * 100) / dyn_ctx[id].total_tsc;
            uint64_t ctx_sw = cpu->sched_stats.context_switches - dyn_ctx[id].last_ctx_sw;
            
            if (idle_ratio > 50) {
                if (cpu->base_quantum < 15) cpu->base_quantum++;
            } else if (idle_ratio < 10 && ctx_sw > 500) {
                if (cpu->base_quantum > 2) cpu->base_quantum--;
            } else {
                if (cpu->base_quantum > 5) cpu->base_quantum--;
                else if (cpu->base_quantum < 5) cpu->base_quantum++;
            }
        }
        
        dyn_ctx[id].idle_tsc = 0;
        dyn_ctx[id].total_tsc = 0;
        dyn_ctx[id].last_ctx_sw = cpu->sched_stats.context_switches;
        dyn_ctx[id].last_adjust_ms = now_ms;
    }
}

static inline uint64_t get_dynamic_quantum(cpu_t *cpu, thread_t *thread) {
    if (!thread || thread == cpu->idle_thread) return cpu->base_quantum;
    if (thread->custom_quantum > 0) return thread->custom_quantum;
    uint64_t weight = thread->weight ? thread->weight : 1024;
    uint64_t quantum = (cpu->base_quantum * weight) / 1024;
    if (quantum < 1) quantum = 1;
    if (quantum > cpu->base_quantum * 8) quantum = cpu->base_quantum * 8;
    return quantum;
}

static void reclaim_zombie_list(cpu_t *cpu, thread_t *head) {
    thread_t *z = head;
    while (z) {
        thread_t *next = z->zombie_next;
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

        uint64_t rflags = spin_lock_irqsave(&cpu->sched_lock);
        if (cpu->zombie_list) {
            zombie_head = cpu->zombie_list;
            cpu->zombie_list = nullptr;
            cpu->zombie_count = 0;
        }
        spin_unlock_irqrestore(&cpu->sched_lock, rflags);

        reclaim_zombie_list(cpu, zombie_head);
        
        Schedule::DrainProcZombieList(cpu);
        
        file_cache_idle_handler(cpu->file_cache);
        asm volatile("sti; hlt; cli" ::: "memory");
    }
}

static inline thread_t* safe_get_current_thread(cpu_t *cpu) {
    thread_t *t = cpu->current_thread;
    if ((uintptr_t)t < 0xFFFF800000000000) {
        kwarn("Invalid current_thread pointer: %p, resetting to idle\n", t);
        t = cpu->idle_thread;
        cpu->current_thread = t;
    }
    return t;
}

cpu_t *get_lw_cpu(cpu_t *ref_cpu = nullptr) {
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

namespace Schedule {
    namespace Internal {
        void RemoveFromQueue(cpu_t *cpu, thread_t *thread) {
            if (!thread->on_rq) return;
            rb_node_t *node = &thread->rb_node;

            rb_erase(&cpu->runqueue_root, node);
            thread->on_rq = false;
            cpu->thread_count--;
            cpu->total_weight -= thread->weight;

            if (node->parent) {
                update_min_vruntime_upward(node->parent);
            }
            
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

        thread_t *StealThread(cpu_t *cpu) {
            uint32_t my_mask = cpu_simd_mask(cpu);
            uint32_t start_cpu = (sched_pid + PIT::TimeSinceBootMS() + per_cpu_steal_cursor[cpu->id]) % (smp_last_cpu + 1);
            per_cpu_steal_cursor[cpu->id]++;

            for (int pass = 0; pass < 2; pass++) {
                for (uint32_t k = 0; k <= smp_last_cpu; k++) {
                    uint32_t i = (start_cpu + k) % (smp_last_cpu + 1);
                    cpu_t *victim = smp_cpu_list[i];
                    if (!victim || victim == cpu) continue;
                    if (pass == 0 && cpu_simd_mask(victim) != my_mask) continue;
                    if (!atomic_load_1(&victim->has_surplus, ATOMIC_RELAXED)) continue;

                    cpu->sched_stats.steal_attempts++;

                    uint64_t rflags1;
                    int retries = 0;
                    while (!spin_trylock_irqsave(&victim->sched_lock, &rflags1)) {
                        if (++retries > 100) break;
                        asm volatile("pause");
                    }
                    if (retries > 100) continue;

                    if (victim->thread_count <= 1) {
                        spin_unlock_irqrestore(&victim->sched_lock, rflags1);
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
                        if (stolen->timer_bucket != nullptr) { node = prev_node; continue; }
                        if (stolen->vruntime > victim->avg_vruntime + SCHED_HUNGER_THRESHOLD) {
                            node = prev_node; 
                            continue;
                        }
                        
                        RemoveFromQueue(victim, stolen);
                        stolen_batch[stolen_count++] = stolen;
                        node = prev_node;
                    }

                    if (stolen_count > 0) {
                        for (int j = 0; j < stolen_count; j++) {
                            __atomic_store_n(&stolen_batch[j]->state, THREAD_TRANSFER, __ATOMIC_RELEASE);
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

                        if (best) {
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

            uint64_t rflags = spin_lock_irqsave(&lock_a->sched_lock);
            uint64_t rflags_b;
            if (!spin_trylock_irqsave(&lock_b->sched_lock, &rflags_b)) {
                spin_unlock_irqrestore(&lock_a->sched_lock, rflags);
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
                if (to_push->timer_bucket != nullptr) { node = prev_node; continue; }
                if (to_push->vruntime > cpu->avg_vruntime + SCHED_HUNGER_THRESHOLD) {
                    node = prev_node; 
                    continue;
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
            spin_unlock_irqrestore(&lock_b->sched_lock, rflags_b);
            spin_unlock_irqrestore(&lock_a->sched_lock, rflags);
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
            const uint64_t avg_vr = cpu->avg_vruntime;

            while (node) {
                if (node->left) {
                    thread_t *lt = rb_to_thread(node->left);
                    if (lt->min_vruntime_subtree <= avg_vr) {
                        node = node->left;
                        continue;
                    }
                }
                thread_t *cur = rb_to_thread(node);
                if (cur->vruntime <= avg_vr) {
                    best = cur;
                    break;
                }
                node = node->right;
            }

            if (!best) best = rb_to_thread(rb_first(cpu->runqueue_root.node));
            if (best) RemoveFromQueue(cpu, best);
            return best;
        }

        void Switch(context_t *ctx) {
            LAPIC::StopTimer();
            cpu_t *cpu = this_cpu();
            if (!cpu) return;
            
            uint64_t rflags = irq_save();

            if (__atomic_load_n(&need_resched_flags[cpu->id], __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&need_resched_flags[cpu->id], false, __ATOMIC_RELEASE);
            }

            if (cpu->preempt_count > 1) {
                if (cpu->current_thread) {
                    uint64_t q = get_dynamic_quantum(cpu, cpu->current_thread);
                    LAPIC::Oneshot(SCHED_VEC, q * cpu->lapic_ticks);
                }
                LAPIC::EOI();
                irq_restore(rflags);
                return;
            }

            cpu->tick_count++;
            thread_t *curr_thread = safe_get_current_thread(cpu);
            uint64_t now = PIT::TimeSinceBootMS();

            dynamic_adjust_quantum(cpu, curr_thread, now);

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
            } else if (curr_thread == cpu->idle_thread) {
                uint64_t delta = now - curr_thread->last_run_time;
                curr_thread->last_run_time = now;
                if (delta > 0) {
                    uint64_t base_weight = 1024;
                    uint64_t avg_total = delta * 1024 + cpu->avg_vruntime_rem;
                    uint64_t avg_delta = avg_total / base_weight;
                    cpu->avg_vruntime_rem = avg_total % base_weight;
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

            thread_t *zombie_to_free = nullptr;
            if (cpu->zombie_count >= ZOMBIE_RECLAIM_THRESHOLD) {
                int moved = 0;
                thread_t *z = cpu->zombie_list;
                while (z && moved < ZOMBIE_RECLAIM_BATCH) {
                    thread_t *next = z->zombie_next;
                    z->zombie_next = zombie_to_free;
                    zombie_to_free = z;
                    z = next;
                    moved++;
                }
                if (zombie_to_free) {
                    cpu->zombie_list = z;
                    cpu->zombie_count -= moved;
                }
            }

            uint32_t curr_state = curr_thread ? curr_thread->state : 0xFFFFFFFF;
            if (curr_thread && curr_state == THREAD_ZOMBIE && curr_thread != cpu->idle_thread) {
                curr_thread->zombie_next = cpu->zombie_list;
                cpu->zombie_list = curr_thread;
                cpu->zombie_count++;
                curr_thread = nullptr;
            } else if (curr_thread && curr_state == THREAD_RUNNING && curr_thread != cpu->idle_thread) {
                InsertToQueue(cpu, curr_thread);
            }

            thread_t *next_thread = Pick(cpu);
            
            spinlock_unlock(&cpu->sched_lock);

            reclaim_zombie_list(cpu, zombie_to_free);

            irq_restore(rflags);

            if ((cpu->tick_count & 0x1F) == 0) TryPush(cpu);

            if (!next_thread) {
                next_thread = StealThread(cpu);
                if (!next_thread) next_thread = cpu->idle_thread;
            }

            rflags = irq_save();

            uint64_t quantum = cpu->base_quantum;
            bool is_switch = (next_thread != curr_thread);
            if (is_switch) {
                cpu->current_thread = next_thread;
                cpu->sched_stats.context_switches++;
                if (next_thread != cpu->idle_thread) next_thread->last_run_time = now;
            }

            if (!is_switch) {
                if (next_thread != cpu->idle_thread) {
                    quantum = get_dynamic_quantum(cpu, next_thread);
                    LAPIC::Oneshot(SCHED_VEC, quantum * cpu->lapic_ticks);
                }
                LAPIC::EOI();
                irq_restore(rflags);
                return;
            }

            *ctx = next_thread->ctx;
            TSS::SetRSP(cpu->id, 0, (void*)next_thread->kernel_rsp);
            cpu->kernel_stack = next_thread->kernel_rsp;

            if (!curr_thread || curr_thread->pagemap != next_thread->pagemap) {
                VMM::SwitchPageMap(next_thread->pagemap);
                //asm volatile("cli");
            }

            cpu->OverLoadableFuncs.WRFSBASE(next_thread->fs);
            if (next_thread->fx_area) {
                cpu->OverLoadableFuncs.LoadSIMDState(next_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
            }

            if (next_thread != cpu->idle_thread) quantum = get_dynamic_quantum(cpu, next_thread);
            else quantum = cpu->base_quantum;
            
            LAPIC::Oneshot(SCHED_VEC, quantum * cpu->lapic_ticks);
            LAPIC::EOI();
            irq_restore(rflags);
        }

        void Preempt(context_t *ctx) { Switch(ctx); }
    }

    void CheckPreempt(context_t *ctx) {
        cpu_t *cpu = this_cpu();
        if (!cpu) return;
        if (__atomic_load_n(&need_resched_flags[cpu->id], __ATOMIC_ACQUIRE) && cpu->preempt_count == 0) {
            asm volatile("int %0" :: "i"(SCHED_VEC + 1));
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
            if (remaining_vr * curr->weight * woked_thread->weight < PREEMPT_THRESHOLD) {
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
        if(!NOT_RUNQ_P){
            NOT_RUNQ_P = (art_tree*)kmalloc(sizeof(art_tree));
            if (art_tree_init(NOT_RUNQ_P) != 0) Panic("ART TREE INIT FAILED!");
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
            uint64_t rflags = spin_lock_irqsave(&cpu->sched_lock);
            Internal::RemoveFromQueue(cpu, idle_t);
            spin_unlock_irqrestore(&cpu->sched_lock, rflags);
            cpu->idle_thread = idle_t;
            
            // 修复：补充上下文切换基线初始化
            dyn_ctx[i].last_adjust_ms = PIT::TimeSinceBootMS();
            dyn_ctx[i].last_ctx_sw = cpu->sched_stats.context_switches;
        }
        atomic_store_8((volatile uint8_t*)&PIT::TickHandle, (uint64_t)(uintptr_t)&PIT::Tick_, 0);
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