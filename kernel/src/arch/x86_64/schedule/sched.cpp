//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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

static uint64_t sched_tid = 0;
static uint64_t sched_pid = 0;
art_tree *pid2proc_tree = nullptr;
spinlock_t PID2PROC_TREE_LOCK = 0;
spinlock_t PROC_LIST_LOCK = 0;      

#define SCHED_STEAL_BATCH 8
#define ZOMBIE_RECLAIM_THRESHOLD 8
#define WAIT_THREAD_MAX_TIMEOUT 100000000ULL

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

// Global average vruntime. Monotonically increasing.
// Represents the system-wide vruntime baseline to ensure fairness across CPUs.
static volatile uint64_t global_avg_vruntime = 0;

void sched_idle() {
    while (true) {
        cpu_t *cpu = this_cpu();
        
        // Reclaim zombie threads when idle
        thread_t *zombie_head = nullptr;
        spinlock_lock(&cpu->sched_lock);
        if (cpu->zombie_list) {
            zombie_head = cpu->zombie_list;
            cpu->zombie_list = nullptr;
            cpu->zombie_count = 0;
        }
        spinlock_unlock(&cpu->sched_lock);

        thread_t *z = zombie_head;
        while (z) {
            thread_t *next = z->zombie_next;
            Schedule::FreeThreadResources(z);
            kfree(z);
            cpu->sched_stats.zombie_reclaims++;
            z = next;
        }
        
        Schedule::PAUSE();
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

static void update_global_avg_vruntime(uint64_t new_val) {
    uint64_t old = __atomic_load_n(&global_avg_vruntime, __ATOMIC_RELAXED);
    while (new_val > old) {
        if (__atomic_compare_exchange_n(&global_avg_vruntime, &old, new_val,
                                        false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            break;
    }
}

static inline void calibrate_and_set_deadline(thread_t *thread, cpu_t *cpu) {
    uint64_t quantum = (thread->custom_quantum > 0) ? thread->custom_quantum : cpu->base_quantum;
    uint64_t max_lag = (quantum * 4 * 1024) / thread->weight;
    
    if (cpu->avg_vruntime > thread->vruntime + max_lag) {
        thread->vruntime = cpu->avg_vruntime - max_lag;
        thread->vruntime_rem = 0; 
    } else if (thread->vruntime > cpu->avg_vruntime + max_lag) {
        thread->vruntime = cpu->avg_vruntime + max_lag;
        thread->vruntime_rem = 0;
    }
    
    thread->deadline = thread->vruntime + (quantum * 1024) / thread->weight;
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
                while (successor->left)
                    successor = successor->left;
                update_1 = successor->parent;
                update_2 = successor;
            }

            rb_erase(&cpu->runqueue_root, node);
            thread->on_rq = false;
            cpu->thread_count--;
            cpu->total_weight -= thread->weight;
            
            if (update_1)
                update_min_vruntime_upward(update_1);
            if (update_2)
                update_min_vruntime_upward(update_2);
            
            if (cpu->thread_count == 1) cpu->has_surplus = false;
        }

        void InsertToQueue(cpu_t *cpu, thread_t *thread) {
            if (thread->on_rq) return; 
            
            if (thread->state == THREAD_ZOMBIE) {
                if (thread->parent) {
                    // Lock order: CPU sched lock -> PROC_LIST_LOCK
                    spinlock_lock(&PROC_LIST_LOCK);
                    detach_thread_from_proc(thread);
                    spinlock_unlock(&PROC_LIST_LOCK);
                }
                thread->zombie_next = cpu->zombie_list;
                cpu->zombie_list = thread;
                cpu->zombie_count++;
                return;
            }
            
            calibrate_and_set_deadline(thread, cpu);
            
            thread->last_run_time = PIT::TimeSinceBootMS();
            
            rb_insert(&cpu->runqueue_root, &thread->rb_node, thread_rb_cmp);
            thread->on_rq = true;
            cpu->thread_count++;
            cpu->total_weight += thread->weight;
            if (cpu->thread_count == 2) cpu->has_surplus = true;
            
            update_min_vruntime_upward(&thread->rb_node);
        }

        void ProcessAddThread(proc_t *parent, thread_t *thread) {
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

                    int retries = 0;
                    while (!__sync_bool_compare_and_swap(&victim->sched_lock,0,1)) {
                        if (++retries > 100) break;
                        asm volatile("pause");
                    }
                    if (retries > 100) continue;

                    if (victim->thread_count <= 1) {
                        spinlock_unlock(&victim->sched_lock);
                        continue;
                    }

                    thread_t *stolen_batch[SCHED_STEAL_BATCH];
                    int stolen_count = 0;

                    while (stolen_count < SCHED_STEAL_BATCH) {
                        if (victim->thread_count <= 1) break;
                        
                        rb_node_t *node = rb_last(victim->runqueue_root.node);
                        if (!node) break;
                        thread_t *stolen = rb_to_thread(node);
                        
                        if (stolen->timer_bucket != nullptr) {
                            if (stolen->timer_cpu == victim->id) {
                                TimerRemove(stolen);
                                stolen->timer_bucket = nullptr; 
                            } else {
                                break; 
                            }
                        }
                        
                        RemoveFromQueue(victim, stolen);
                        
                        // If a zombie thread is encountered during stealing, 
                        // attach it to the victim's zombie list directly.
                        if (stolen->state == THREAD_ZOMBIE) {
                            if (stolen->parent) {
                                // Lock order: CPU sched lock -> PROC_LIST_LOCK
                                spinlock_lock(&PROC_LIST_LOCK);
                                detach_thread_from_proc(stolen);
                                spinlock_unlock(&PROC_LIST_LOCK);
                            }
                            stolen->zombie_next = victim->zombie_list;
                            victim->zombie_list = stolen;
                            victim->zombie_count++;
                            continue;
                        }
                        
                        stolen_batch[stolen_count++] = stolen;
                    }

                    if (stolen_count > 0) {
                        spinlock_unlock(&victim->sched_lock);
                        
                        spinlock_lock(&cpu->sched_lock);
                        // InsertToQueue handles zombie state safely.
                        // All stolen threads are inserted into the local queue.
                        for (int j = 0; j < stolen_count; j++) {
                            stolen_batch[j]->cpu_num = cpu->id;
                            stolen_batch[j]->timer_cpu = cpu->id;
                            InsertToQueue(cpu, stolen_batch[j]);
                        }
                        
                        // Pick the best from the local queue to return.
                        // Pick safely ignores zombies.
                        thread_t *best = Pick(cpu);
                        spinlock_unlock(&cpu->sched_lock);

                        if (best) {
                            cpu->sched_stats.thread_steals += stolen_count;
                            return best;
                        }
                        
                        // If best is null (e.g. all stolen became zombies and queue is empty),
                        // return null and let Switch handle idle.
                        return nullptr;
                    }
                    spinlock_unlock(&victim->sched_lock);
                }
            }
            return nullptr;
        }

        void TryPush(cpu_t *cpu) {
            cpu->sched_stats.try_pushes++; // Count total invocations

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
                if (ow * 3 < my_weight * 2 && ow < target_weight) {
                    if (cpu_simd_mask(other) == my_mask) {
                        target = other;
                        target_weight = ow;
                    } else if (!fallback_target) {
                        fallback_target = other;
                    }
                }
            }
            
            if (!target) {
                if (fallback_target) target = fallback_target;
                else return;
            }

            // Strict lock ordering by CPU ID to prevent deadlocks
            cpu_t *lock_a = (cpu->id < target->id) ? cpu : target;
            cpu_t *lock_b = (cpu->id < target->id) ? target : cpu;
            
            spinlock_lock(&lock_a->sched_lock);
            // Use non-blocking trylock for the second lock to avoid extending interrupt latency
            if (!__sync_bool_compare_and_swap(&lock_b->sched_lock, 0, 1)) {
                spinlock_unlock(&lock_a->sched_lock);
                return;
            }

            int push_count = 0;
            while (push_count < SCHED_STEAL_BATCH) {
                uint64_t tc_w = target->total_weight + (target->current_thread ? target->current_thread->weight : 0);
                uint64_t mc_w = cpu->total_weight + (cpu->current_thread ? cpu->current_thread->weight : 0);
                if (tc_w * 3 >= mc_w * 2) break;

                rb_node_t *node = rb_last(cpu->runqueue_root.node);
                if (!node) break;
                thread_t *to_push = rb_to_thread(node);
                
                if (to_push->timer_bucket != nullptr) {
                    if (to_push->timer_cpu == cpu->id) {
                        TimerRemove(to_push);
                        to_push->timer_bucket = nullptr; 
                    } else {
                        break; 
                    }
                }
                
                RemoveFromQueue(cpu, to_push);
                
                to_push->cpu_num = target->id;
                to_push->timer_cpu = target->id;
                
                InsertToQueue(target, to_push);
                push_count++;
            }

            cpu->sched_stats.push_success += push_count;

            spinlock_unlock(&lock_b->sched_lock);
            spinlock_unlock(&lock_a->sched_lock);
        }

        thread_t *Pick(cpu_t *cpu) {
            rb_node_t *root = cpu->runqueue_root.node;
            if (!root) return nullptr;
            
            thread_t *root_t = rb_to_thread(root);
            if (root_t->min_vruntime_subtree > cpu->avg_vruntime) {
                thread_t *best = rb_to_thread(rb_first(root));
                if (best) {
                    RemoveFromQueue(cpu, best);
                    return best;
                }
                return nullptr;
            }

            rb_node_t *node = root;
            thread_t *best = nullptr;
            
            while (node) {
                thread_t *t = rb_to_thread(node);
                
                if (t->vruntime <= cpu->avg_vruntime) {
                    best = t;
                    node = node->left;
                } else {
                    if (node->left) {
                        thread_t *lt = rb_to_thread(node->left);
                        if (lt->min_vruntime_subtree <= cpu->avg_vruntime) {
                            node = node->left;
                            continue;
                        }
                    }
                    node = node->right;
                }
            }
            
            if (!best) {
                best = rb_to_thread(rb_first(cpu->runqueue_root.node));
            }
            
            if (best) {
                RemoveFromQueue(cpu, best);
            }
            return best;
        }

        void Switch(context_t *ctx) {
            LAPIC::StopTimer();
            cpu_t *cpu = this_cpu();
            if (!cpu) return;

            if (cpu->preempt_count > 1) {
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                return;
            }

            cpu->tick_count++;
            thread_t *curr_thread = cpu->current_thread;

            uint64_t now = PIT::TimeSinceBootMS();
            if (curr_thread && curr_thread != cpu->idle_thread) {
                uint64_t delta = now - curr_thread->last_run_time;
                curr_thread->last_run_time = now;
                
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
                update_global_avg_vruntime(cpu->avg_vruntime);
            }

            if (curr_thread && curr_thread != cpu->idle_thread) {
                curr_thread->fs = rdmsr(FS_BASE);
                curr_thread->ctx = *ctx;
                if (curr_thread->fx_area) {
                    cpu->OverLoadableFuncs.StoreSIMDState(
                        curr_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
                }
            }

            spinlock_lock(&cpu->sched_lock);

            uint32_t curr_state = curr_thread ? curr_thread->state : 0xFFFFFFFF;

            if (curr_thread && curr_state == THREAD_ZOMBIE && curr_thread != cpu->idle_thread) {
                cpu->current_thread = nullptr; 
                
                if (curr_thread->parent) {
                    // Lock order: CPU sched lock -> PROC_LIST_LOCK
                    spinlock_lock(&PROC_LIST_LOCK);
                    detach_thread_from_proc(curr_thread);
                    spinlock_unlock(&PROC_LIST_LOCK);
                }
                
                curr_thread->zombie_next = cpu->zombie_list;
                cpu->zombie_list = curr_thread;
                cpu->zombie_count++;
                curr_thread = nullptr;
            } else if (curr_thread && curr_state == THREAD_RUNNING
                && curr_thread != cpu->idle_thread) {
                InsertToQueue(cpu, curr_thread);
            }

            thread_t *next_thread = Pick(cpu);
            
            thread_t *zombie_to_free = nullptr;
            if (((cpu->tick_count & 0xF) == 0 || cpu->zombie_count >= ZOMBIE_RECLAIM_THRESHOLD) && cpu->zombie_list) {
                zombie_to_free = cpu->zombie_list;
                cpu->zombie_list = nullptr;
                cpu->zombie_count = 0;
            }
            
            spinlock_unlock(&cpu->sched_lock);

            if (zombie_to_free) {
                thread_t *z = zombie_to_free;
                while (z) {
                    thread_t *next = z->zombie_next;
                    FreeThreadResources(z);
                    kfree(z);
                    cpu->sched_stats.zombie_reclaims++;
                    z = next;
                }
            }

            if (!next_thread) {
                next_thread = StealThread(cpu);
                if (!next_thread) next_thread = cpu->idle_thread;
            }

            if (!next_thread) {
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                return;
            }

            // Redundant defensive check removed: StealThread and Pick guarantee non-zombie.

            if (next_thread == cpu->idle_thread) {
                uint64_t g_avg = __atomic_load_n(&global_avg_vruntime, __ATOMIC_ACQUIRE);
                if (cpu->avg_vruntime < g_avg) {
                    cpu->avg_vruntime = g_avg;
                    cpu->avg_vruntime_rem = 0; 
                }
            }

            bool is_switch = (next_thread != curr_thread);
            if (is_switch) {
                cpu->current_thread = next_thread;
                cpu->sched_stats.context_switches++;
                if (next_thread != cpu->idle_thread) {
                    next_thread->last_run_time = now;
                }
            }

            // Active load balancing, executed outside the sched lock.
            // Triggered every 32 ticks to avoid excessive overhead.
            if ((cpu->tick_count & 0x1F) == 0) {
                TryPush(cpu);                
            }

            if (!is_switch) {
                if (next_thread != cpu->idle_thread) {
                    uint64_t q = (next_thread->custom_quantum > 0) ? next_thread->custom_quantum : cpu->base_quantum;
                    LAPIC::Oneshot(SCHED_VEC, q);
                }
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
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
            
            if (next_thread != cpu->idle_thread) {
                uint64_t quantum = (next_thread->custom_quantum > 0) ? next_thread->custom_quantum : cpu->base_quantum;
                LAPIC::Oneshot(SCHED_VEC, quantum);
            }
            
            if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
        }

        void Preempt(context_t *ctx) {
            Switch(ctx);
        }
    }

    /*
     * Asynchronous thread termination interface.
     * Callers must NOT free the thread object immediately after calling this function.
     * The thread might still be running on another CPU. It will be safely reclaimed
     * by the scheduler's zombie list mechanism in the future.
     * If the thread is running on another CPU, an IPI will be sent to force a
     * schedule switch to expedite the reclamation process.
     */
    void KillThread(thread_t *thread) {
        if (!thread) return;
        cpu_t *cpu = get_cpu(thread->cpu_num);
        bool was_running = false;
        
        spinlock_lock(&cpu->sched_lock);
        thread->state = THREAD_ZOMBIE;
        was_running = (cpu->current_thread == thread);
        
        // Safely detach from process list immediately to prevent UAF
        // Lock order: CPU sched lock -> PROC_LIST_LOCK
        spinlock_lock(&PROC_LIST_LOCK);
        detach_thread_from_proc(thread);
        spinlock_unlock(&PROC_LIST_LOCK);

        if (thread->on_rq) {
            Internal::RemoveFromQueue(cpu, thread);
            thread->zombie_next = cpu->zombie_list;
            cpu->zombie_list = thread;
            cpu->zombie_count++;
        }
        spinlock_unlock(&cpu->sched_lock);

        // If the thread is currently running on another CPU, send an IPI
        // to force a schedule switch so it can be safely reclaimed.
        if (was_running) {
            cpu_t *cur_cpu = this_cpu();
            if (cpu != cur_cpu) {
                LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
            }
        }
    }

    /*
     * Batch terminate all threads belonging to a process.
     * Reads the thread list in batches to avoid holding the process lock for too long
     * and to support processes with a large number of threads.
     */
    void KillProcessThreads(proc_t *proc) {
        if (!proc) return;
        
        while (true) {
            thread_t *batch[64];
            int count = 0;
            
            spinlock_lock(&PROC_LIST_LOCK);
            thread_t *t = proc->threads;
            if (t) {
                thread_t *start = t;
                do {
                    if (count < 64) {
                        batch[count++] = t;
                        t = t->next;
                    } else {
                        break;
                    }
                } while (t != start);
            }
            spinlock_unlock(&PROC_LIST_LOCK);

            if (count == 0) break;

            for (int i = 0; i < count; i++) {
                KillThread(batch[i]);
            }
        }
    }

    /*
     * Synchronously wait for a thread to be off the CPU.
     * Uses atomic loads with acquire semantics for safe cross-core visibility.
     * Includes a timeout to prevent permanent hangs in case of bugs.
     */
    void WaitForThreadOffCpu(thread_t *thread) {
        if (!thread) return;
        uint64_t timeout = 0;
        
        while (timeout < WAIT_THREAD_MAX_TIMEOUT) {
            // Use atomic load for cpu_num as it might change during migration
            uint32_t cpu_num = __atomic_load_n(&thread->cpu_num, __ATOMIC_ACQUIRE);
            if (cpu_num >= MAX_CPU) break; 
            cpu_t *cpu = smp_cpu_list[cpu_num];
            if (!cpu) break;
            
            // Use atomic load for current_thread to ensure visibility
            thread_t *curr = __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE);
            if (curr != thread) {
                break;
            }
            asm volatile("pause");
            timeout++;
        }
        
        if (timeout >= WAIT_THREAD_MAX_TIMEOUT) {
            Panic("WaitForThreadOffCpu: Thread stuck on CPU (timeout)");
        }
    }

    /*
     * Wakeup preempt interface.
     * Triggered after a thread is woken up and inserted into a CPU's runqueue.
     * Checks if the woken thread's deadline is earlier than the currently running
     * thread on that CPU. If so, sends an IPI to force a schedule switch,
     * improving response latency for interactive tasks.
     * Note: In extreme high-frequency wakeup scenarios, adding a throttle mechanism
     * (e.g., minimum preempt interval) could be considered to prevent IPI storms.
     */
    void TriggerPreempt(thread_t *woken_thread) {
        if (!woken_thread) return;
        
        uint32_t cpu_num = __atomic_load_n(&woken_thread->cpu_num, __ATOMIC_ACQUIRE);
        if (cpu_num >= MAX_CPU) return;
        cpu_t *cpu = smp_cpu_list[cpu_num];
        if (!cpu) return;

        thread_t *curr = __atomic_load_n(&cpu->current_thread, __ATOMIC_ACQUIRE);
        
        // If the CPU is idle, simply send an IPI to wake it up.
        if (!curr || curr == cpu->idle_thread) {
            LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
            return;
        }

        // If the woken thread has an earlier deadline than the currently running thread,
        // it should preempt the current thread to maintain EEVDF scheduling semantics.
        if (woken_thread->deadline < curr->deadline) {
            cpu_t *cur_cpu = this_cpu();
            if (cpu != cur_cpu) {
                LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
            } else {
                // If it's the current CPU, force a yield
                Yield();
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
        idt_set_ist(SCHED_VEC, 1);
        idt_set_ist(SCHED_VEC + 1, 1);
    }
    
    void Install() {
        for (uint32_t i = 0; i <= smp_last_cpu; i++) {
            cpu_t *cpu = smp_cpu_list[i];
            if (!cpu) continue;
            
            proc_t *proc = Schedule::NewProcess(false);
            thread_t *idle_t = Schedule::NewKernelThread(proc, cpu->id, 15, sched_idle);
            
            spinlock_lock(&cpu->sched_lock);
            Internal::RemoveFromQueue(cpu, idle_t);
            spinlock_unlock(&cpu->sched_lock);
            
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
        if (!proc->FDMan) { 
            if (user) VMM::DestroyPM(proc->pagemap); 
            kfree(proc); 
            return nullptr; 
        }
        fd_manager_init(proc->FDMan);
        proc->fd_count = 4;
        
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_insert(pid2proc_tree,(const uint8_t*)&proc->id,8,proc);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        return proc;
    }

    void PrepareUserStack(thread_t *thread, int32_t argc, char *argv[], char *envp[]) {
        if (argc <= 0 || !argv || !envp) return;
        
        char **kernel_argv = nullptr;
        char **kernel_envp = nullptr;
        uint64_t *thread_argv = nullptr;
        uint64_t *thread_envp = nullptr;
        int32_t envc = 0;uint64_t offset = 0;
        uint64_t stack_top = 0;pagemap_t *restore = nullptr;
        
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
            if(!thread_envp) {
                VMM::SwitchPageMap(restore);
                goto cleanup;
            }
            for (int32_t i = 0; i < envc; i++) {
                int32_t size = strlen(kernel_envp[i]) + 1;
                offset += ALIGN_UP(size, 16);
                thread_envp[i] = stack_top - offset;
                __memcpy((void*)(stack_top - offset), kernel_envp[i], size);
            }
        }
        
        offset += 8; *(uint64_t*)(stack_top - offset) = 0; 
        for (int32_t i = envc - 1; i >= 0; i--) {
            offset += 8; *(uint64_t*)(stack_top - offset) = thread_envp[i];
        }
        offset += 8; *(uint64_t*)(stack_top - offset) = 0; 
        for (int32_t i = argc - 1; i >= 0; i--) {
            offset += 8; *(uint64_t*)(stack_top - offset) = thread_argv[i];
        }
        offset += 8; *(uint64_t*)(stack_top - offset) = argc;
        VMM::SwitchPageMap(restore);
        thread->ctx.rsp = stack_top - offset;

    cleanup:
        if (kernel_argv) {
            for (int32_t i = 0; i < argc; i++) if (kernel_argv[i]) kfree(kernel_argv[i]);
            kfree(kernel_argv);
        }
        if (kernel_envp) {
            for (int32_t i = 0; i < envc; i++) if (kernel_envp[i]) kfree(kernel_envp[i]);
            kfree(kernel_envp);
        }
        if (thread_argv) kfree(thread_argv);
        if (thread_envp) kfree(thread_envp);
    }

    thread_t *NewKernelThread(proc_t *parent, uint32_t cpu_num, int32_t priority, void *entry) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        
        thread->timer_bucket = nullptr;
        thread->timer_next = nullptr;
        thread->timer_prev = nullptr;
        thread->timer_cpu = cpu_num;
        thread->timer_wakeup = 0;
        
        thread->id = atomic_add_fetch_8(&sched_tid,1,ATOMIC_RELAXED);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->IsForkThread = false;
        thread->pagemap = parent->pagemap;
        
        if (priority > 15) priority = 15;
        thread->priority = priority;
        thread->weight = sched_prio_to_weight[priority];
        
        cpu_t *cpu = get_cpu(cpu_num);
        
        uint64_t g_avg = __atomic_load_n(&global_avg_vruntime, __ATOMIC_ACQUIRE);
        uint64_t base_vruntime = (cpu->avg_vruntime > g_avg) ? cpu->avg_vruntime : g_avg;
        uint64_t half_slice = (cpu->base_quantum * 1024) / (2 * thread->weight);
        thread->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;
        
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((cpu->XsaveSize), PAGE_SIZE), true);
        if (!thread->fx_area) { kfree(thread); return nullptr; }
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, thread->fx_area); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);

        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);
        thread->stack = kernel_stack;
        thread->ctx.rip = (uint64_t)entry;
        thread->ctx.cs = 0x08;
        thread->ctx.ss = 0x10;
        thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread->kernel_rsp;
        thread->thread_stack = thread->ctx.rsp;
        thread->state = THREAD_RUNNING;

        Schedule::Internal::ProcessAddThread(parent, thread);

        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);

        return thread;
    }

    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int32_t priority, const char *Path, int32_t argc, char *argv[], char *envp[]) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));

        thread->timer_bucket = nullptr;
        thread->timer_next = nullptr;
        thread->timer_prev = nullptr;
        thread->timer_cpu = cpu_num;
        thread->timer_wakeup = 0;

        thread->id = atomic_add_fetch_8(&sched_tid,1,ATOMIC_RELAXED);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->pagemap = parent->pagemap;
        
        if (priority > 15) priority = 15;
        thread->priority = priority;
        thread->weight = sched_prio_to_weight[priority];
        
        cpu_t *cpu = get_cpu(cpu_num);
        
        uint64_t g_avg = __atomic_load_n(&global_avg_vruntime, __ATOMIC_ACQUIRE);
        uint64_t base_vruntime = (cpu->avg_vruntime > g_avg) ? cpu->avg_vruntime : g_avg;
        uint64_t half_slice = (cpu->base_quantum * 1024) / (2 * thread->weight);
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
        MP->FSOPS->close(FileDesc);
        kfree(FileDesc);
        
        uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
        _memset(&thread->ctx, 0, sizeof(context_t));
        thread->ctx.rip = elf_load(buffer, thread->pagemap, &tls_offset, &tls_memsz, &tls_filesz, &tls_align); 
        
        if (thread->ctx.rip == 0) {
            kerrorln("ELF load failed!");
            kfree(buffer); kfree(thread); return nullptr; 
        }

        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        if (!thread->fx_area) { kfree(buffer); kfree(thread); return nullptr; }
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, thread->fx_area); kfree(buffer); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);

        uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
        if (!thread_stack) { VMM::Free(kernel_pagemap, thread->fx_area); VMM::Free(kernel_pagemap, (void*)kernel_stack); kfree(buffer); kfree(thread); return nullptr; }
        thread->stack = thread_stack;
        thread->thread_stack = thread_stack + 8 * PAGE_SIZE;

        uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
        if (!sig_stack) { VMM::Free(kernel_pagemap, thread->fx_area); VMM::Free(kernel_pagemap, (void*)kernel_stack); VMM::Free(thread->pagemap, (void*)thread_stack); kfree(buffer); kfree(thread); return nullptr; }
        thread->sig_stack = sig_stack;

        thread->ctx.cs = 0x1b;
        thread->ctx.ss = 0x23;
        thread->ctx.rflags = 0x202;
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
            thread->fs = tcb_base;
            thread->tls_base = tls_mem;
            thread->tls_pages = tls_pages;
        }

        kfree(buffer);
        thread->state = THREAD_RUNNING;

        Schedule::Internal::ProcessAddThread(parent, thread);

        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);

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

        spinlock_lock(&parent_cpu->sched_lock);
        if (parent_cpu->current_thread == parent && parent->fx_area) {
            parent_cpu->OverLoadableFuncs.StoreSIMDState(
                parent->fx_area, parent_cpu->XsaveMaskLo, parent_cpu->XsaveMaskHi);
        }
        __memcpy(thread->fx_area, parent->fx_area, cpu->XsaveSize);
        spinlock_unlock(&parent_cpu->sched_lock);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, thread->fx_area); kfree(thread); return nullptr; }
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);

        thread->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
        thread->cpu_num = cpu->id;
        thread->parent = proc;
        thread->IsForkThread = true;
        thread->pagemap = proc->pagemap;
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + 4 * PAGE_SIZE;
        
        thread->stack = parent->stack;
        thread->sig_stack = parent->sig_stack;
        thread->tls_base = parent->tls_base;
        thread->tls_pages = parent->tls_pages;

        thread->timer_bucket = nullptr;
        thread->timer_next = nullptr;
        thread->timer_prev = nullptr;
        thread->timer_cpu = cpu->id;
        thread->timer_wakeup = 0;

        Schedule::Internal::ProcessAddThread(proc, thread);
        __memcpy(&thread->ctx, frame, sizeof(context_t));
        
        thread->ctx.rsp = ((context_t*)frame)->rsp; 
        
        thread->ctx.cs = 0x1b;
        thread->ctx.ss = 0x23;
        thread->ctx.rflags = ((syscall_frame_t*)frame)->r11;
        thread->ctx.rax = 0;
        thread->ctx.rip = ((syscall_frame_t*)frame)->rcx;
        thread->thread_stack = thread->ctx.rsp;
        thread->fs = rdmsr(FS_BASE);
        thread->state = THREAD_RUNNING;
        
        thread->priority = parent->priority;
        thread->weight = parent->weight;
        uint64_t g_avg = __atomic_load_n(&global_avg_vruntime, __ATOMIC_ACQUIRE);
        uint64_t base_vruntime = (cpu->avg_vruntime > g_avg) ? cpu->avg_vruntime : g_avg;
        uint64_t half_slice = (cpu->base_quantum * 1024) / (2 * thread->weight);
        thread->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;

        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);

        return thread;
    }

    proc_t *ForkProcess() {
        proc_t *parent = this_proc();
        if (!parent) return nullptr;
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        if (!proc) return nullptr;
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid,1,ATOMIC_RELAXED);
        proc->parent = parent;
        
        proc->pagemap = VMM::Fork(parent->pagemap);
        if (!proc->pagemap) { kfree(proc); return nullptr; }
        
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        if (!proc->FDMan) { VMM::DestroyPM(proc->pagemap); kfree(proc); return nullptr; }

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
        
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_insert(pid2proc_tree, (const uint8_t*)&proc->id, 8, proc);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        
        return proc;
    }

    thread_t* this_thread() {
        cpu_t* cpu = this_cpu();
        return cpu ? cpu->current_thread : nullptr;
    }

    proc_t *this_proc() {
        thread_t* t = this_thread();
        return t ? t->parent : nullptr;
    }
    
    void Yield() {
        LAPIC::StopTimer();
        asm volatile("int %0" :: "i"(SCHED_VEC + 1));
    }

    void PAUSE() { LAPIC::StopTimer(); }
    
    void Resume() {
        cpu_t* cur_cpu = this_cpu();
        if (!cur_cpu) return;
        
        for (int32_t i = 0; i <= smp_last_cpu; i++) {
            if (smp_cpu_list[i] && i != cur_cpu->id) {
                LAPIC::IPI(smp_cpu_list[i]->lapic_id, SCHED_VEC + 1);
            }
        }
        LAPIC::IPI(cur_cpu->lapic_id, SCHED_VEC + 1); 
    }
}