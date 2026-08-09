// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <elf/elf.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>
#include <klib/algorithm/art.h>

extern art_tree *pid2proc_tree;
extern spinlock_t PID2PROC_TREE_LOCK;
extern spinlock_t PROC_LIST_LOCK;

#ifndef THREAD_TRANSFER
#define THREAD_TRANSFER 4
#endif

static inline void wait_for_transfer(thread_t *t) {
    uint64_t wait_start = PIT::TimeSinceBootMS();
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == THREAD_TRANSFER) {
        if (PIT::TimeSinceBootMS() - wait_start > 1000) { 
            Panic("Thread stuck in TRANSFER state for too long!");
        }
        asm volatile("pause");
    }
}

static void kill_thread_batch(thread_t *target, cpu_t *self_cpu, bool &need_wait) {
    while (true) {
        wait_for_transfer(target);
        
        uint32_t target_cpu_num = __atomic_load_n(&target->cpu_num, __ATOMIC_ACQUIRE);
        if (target_cpu_num >= MAX_CPU || !smp_cpu_list[target_cpu_num]) {
            // Transient migration state, retry instead of fallback to prevent cross-CPU list corruption
            asm volatile("pause");
            continue; 
        }
        cpu_t *t_cpu = smp_cpu_list[target_cpu_num];

        uint32_t timer_cpu_num = MAX_CPU;
        if (target->timer_bucket != nullptr) {
            timer_cpu_num = __atomic_load_n(&target->timer_cpu, __ATOMIC_ACQUIRE);
        }
        cpu_t *timer_cpu = (timer_cpu_num < MAX_CPU) ? smp_cpu_list[timer_cpu_num] : nullptr;

        bool need_ipi = false;
        uint64_t rflags;
        if (timer_cpu && timer_cpu != t_cpu) {
            // Prevent ABBA deadlock by enforcing global lock order (lower CPU ID first)
            cpu_t *lock1 = (t_cpu->id < timer_cpu->id) ? t_cpu : timer_cpu;
            cpu_t *lock2 = (t_cpu->id < timer_cpu->id) ? timer_cpu : t_cpu;

            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&lock1->sched_lock);
            spinlock_lock(&lock2->sched_lock);

            // Re-validate state, target CPU, and timer CPU under locks to prevent race conditions
            uint32_t cur_target_cpu = __atomic_load_n(&target->cpu_num, __ATOMIC_ACQUIRE);
            uint32_t cur_timer_cpu = __atomic_load_n(&target->timer_cpu, __ATOMIC_ACQUIRE);
            
            if (__atomic_load_n(&target->state, __ATOMIC_ACQUIRE) == THREAD_ZOMBIE ||
                cur_target_cpu != target_cpu_num ||
                (target->timer_bucket != nullptr && cur_timer_cpu != timer_cpu_num)) {
                spinlock_unlock(&lock2->sched_lock);
                spinlock_unlock(&lock1->sched_lock);
                asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
                continue;
            }

            target->state = THREAD_ZOMBIE;
            target->pagemap = kernel_pagemap;

            if (target->timer_bucket != nullptr && cur_timer_cpu == timer_cpu->id) {
                Schedule::Internal::TimerRemove(target);
            }
            if (target->on_rq) {
                Schedule::Internal::RemoveFromQueue(t_cpu, target);
            }

            if (t_cpu->current_thread == target) {
                need_wait = true;
                need_ipi = (t_cpu != self_cpu);
            } else {
                target->zombie_next = t_cpu->zombie_list;
                t_cpu->zombie_list = target;
                t_cpu->zombie_count++;
            }

            spinlock_unlock(&lock2->sched_lock);
            spinlock_unlock(&lock1->sched_lock);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        } else {
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&t_cpu->sched_lock);

            uint32_t cur_target_cpu = __atomic_load_n(&target->cpu_num, __ATOMIC_ACQUIRE);
            uint32_t cur_timer_cpu = __atomic_load_n(&target->timer_cpu, __ATOMIC_ACQUIRE);

            if (__atomic_load_n(&target->state, __ATOMIC_ACQUIRE) == THREAD_ZOMBIE ||
                cur_target_cpu != target_cpu_num ||
                (target->timer_bucket != nullptr && cur_timer_cpu != target_cpu_num)) {
                spinlock_unlock(&t_cpu->sched_lock);
                asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
                continue;
            }

            target->state = THREAD_ZOMBIE;
            target->pagemap = kernel_pagemap;

            if (target->on_rq) {
                Schedule::Internal::RemoveFromQueue(t_cpu, target);
            } else if (target->timer_bucket) {
                Schedule::Internal::TimerRemove(target);
            }

            if (t_cpu->current_thread == target) {
                need_wait = true;
                need_ipi = (t_cpu != self_cpu);
            } else {
                target->zombie_next = t_cpu->zombie_list;
                t_cpu->zombie_list = target;
                t_cpu->zombie_count++;
            }
            spinlock_unlock(&t_cpu->sched_lock);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        }

        // Send IPI outside the sched_lock to reduce lock holding time
        if (need_ipi) {
            LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC + 1);
        }
        return;
    }
}

namespace Schedule {
    void FreeThreadResources(thread_t *thread) {
        if (thread->timer_bucket != nullptr) {
            uint32_t timer_cpu_num = __atomic_load_n(&thread->timer_cpu, __ATOMIC_ACQUIRE);
            if (timer_cpu_num < MAX_CPU) {
                cpu_t *timer_cpu = smp_cpu_list[timer_cpu_num];
                if (timer_cpu) {
                    uint64_t rflags;
                    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
                    spinlock_lock(&timer_cpu->sched_lock);
                    if (thread->timer_bucket != nullptr && __atomic_load_n(&thread->timer_cpu, __ATOMIC_ACQUIRE) == timer_cpu_num) {
                        Schedule::Internal::TimerRemove(thread);
                    }
                    spinlock_unlock(&timer_cpu->sched_lock);
                    asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
                }
            }
        }
        cpu_t *cpu = get_cpu(thread->cpu_num);
        if (thread->fx_area) VMM::Free(kernel_pagemap, thread->fx_area);
        if (thread->kernel_stack) VMM::Free(kernel_pagemap, thread->kernel_stack);
        if (!thread->IsForkThread) {
            if (thread->pagemap != kernel_pagemap) {
                if (thread->stack && thread->stack != thread->kernel_stack) VMM::Free(thread->pagemap, thread->stack);
                if (thread->sig_stack) VMM::Free(thread->pagemap, thread->sig_stack);
                if (thread->tls_base) VMM::Free(thread->pagemap, thread->tls_base);
            }
        }
    }

    static void SyncKillProcThreads(proc_t *proc, thread_t *except_thread) {
        if (!proc) return;
        cpu_t *self_cpu = this_cpu();

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
                    thread_t *next = t->next;
                    if (t != except_thread && count < 64) {
                        if (t->next == t) {
                            proc->threads = nullptr;
                        } else {
                            if (proc->threads == t) proc->threads = t->next;
                            t->prev->next = t->next;
                            t->next->prev = t->prev;
                        }
                        t->parent = nullptr;
                        t->next = t->prev = nullptr;
                        batch[count++] = t;
                    }
                    t = next;
                } while (t != start && proc->threads);
            }
            spinlock_unlock(&PROC_LIST_LOCK);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

            if (count == 0) break;

            bool need_wait[64] = {false};

            for (int i = 0; i < count; i++) {
                kill_thread_batch(batch[i], self_cpu, need_wait[i]);
            }

            for (int i = 0; i < count; i++) {
                if (need_wait[i]) {
                    Schedule::WaitForThreadOffCpu(batch[i]);
                }
            }
        }
    }

    static proc_t* DestroyProcResources(proc_t *proc) {
        uint64_t rflags;
        proc_t *to_delete_list = nullptr;

        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_delete(pid2proc_tree, proc->id, 8);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&PROC_LIST_LOCK);
        proc_t *child = proc->children;
        proc_t *prev = nullptr;
        while (child) {
            proc_t *next_child = child->sibling;
            if (__sync_lock_test_and_set(&child->exiting, 1) == 0) {
                child->parent = nullptr;
                child->sibling = nullptr;
                if (!to_delete_list) {
                    to_delete_list = child;
                    prev = child;
                } else {
                    prev->sibling = child;
                    prev = child;
                }
            } else {
                child->parent = nullptr;
            }
            child = next_child;
        }
        proc->children = nullptr;
        spinlock_unlock(&PROC_LIST_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        if (proc->FDMan) { fd_manager_destroy(proc->FDMan); kfree(proc->FDMan); proc->FDMan = nullptr; }
        if (proc->pagemap && proc->pagemap != kernel_pagemap) {
            VMM::DestroyPM(proc->pagemap);
            proc->pagemap = nullptr;
        }

        kfree(proc);
        return to_delete_list;
    }

    // Core iterative destruction logic, decoupled from permission checks and re-used by all paths
    static void DestroyProcList(proc_t *proc_list) {
        proc_t *to_delete_list = proc_list;
        while (to_delete_list) {
            proc_t *curr = to_delete_list;
            to_delete_list = curr->sibling;
            curr->sibling = nullptr;

            SyncKillProcThreads(curr, nullptr);
            proc_t *new_children = DestroyProcResources(curr);
            
            // Prepend new children to the pending list (iterative, no recursion)
            if (new_children) {
                proc_t *tail = new_children;
                while (tail->sibling) {
                    tail = tail->sibling;
                }
                tail->sibling = to_delete_list;
                to_delete_list = new_children;
            }
        }
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;
        
        // Atomically acquire destruction rights first to prevent UAF races
        if (__sync_lock_test_and_set(&proc->exiting, 1) != 0) return;

        uint64_t rflags;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&PROC_LIST_LOCK);
        if (proc->parent) {
            if (proc->parent->children == proc) proc->parent->children = proc->sibling;
            else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) sibling = sibling->sibling;
                if (sibling) sibling->sibling = proc->sibling;
            }
            proc->parent = nullptr;
            proc->sibling = nullptr;
        }
        spinlock_unlock(&PROC_LIST_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        DestroyProcList(proc);
    }

    static void FinalizeProcExit(proc_t *proc, cpu_t *cpu) {
        uint64_t pid = proc->id;
        thread_t *curr_thread = cpu->current_thread;
        
        SyncKillProcThreads(proc, curr_thread);

        uint64_t rflags;
        // Merge PROC_LIST_LOCK critical sections to detach thread and proc from lists
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&PROC_LIST_LOCK);
        
        if (curr_thread->next == curr_thread) proc->threads = nullptr;
        else {
            if (proc->threads == curr_thread) proc->threads = curr_thread->next;
            curr_thread->prev->next = curr_thread->next;
            curr_thread->next->prev = curr_thread->prev;
        }
        curr_thread->parent = nullptr;
        curr_thread->next = curr_thread->prev = nullptr;

        if (proc->parent) {
            if (proc->parent->children == proc) proc->parent->children = proc->sibling;
            else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) sibling = sibling->sibling;
                if (sibling) sibling->sibling = proc->sibling;
            }
            proc->parent = nullptr;
            proc->sibling = nullptr;
        }
        
        spinlock_unlock(&PROC_LIST_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        pagemap_t *pm_to_destroy = proc->pagemap;
        proc->pagemap = nullptr;
        curr_thread->pagemap = kernel_pagemap;
        curr_thread->stack = 0;
        curr_thread->sig_stack = 0;
        curr_thread->tls_base = 0;

        VMM::SwitchPageMap(kernel_pagemap);
        if (pm_to_destroy && pm_to_destroy != kernel_pagemap) {
            VMM::DestroyPM(pm_to_destroy);
        }

        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&cpu->sched_lock);
        curr_thread->state = THREAD_ZOMBIE;
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        kinfoln("Delete PROC %d", pid);

        proc_t *children = DestroyProcResources(proc);
        
        asm volatile("sti");
        // Re-use the unified iterative destruction logic
        DestroyProcList(children);

        LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
        while(true) { asm volatile("hlt"); }
    }

    void PROC_KILL(proc_t *proc, int32_t exit_code){
        thread_t *curr_thread = Schedule::this_thread();
        cpu_t *cpu = this_cpu();
        curr_thread->exit_code = exit_code;
        
        if (__sync_lock_test_and_set(&proc->exiting, 1) != 0) {
            VMM::SwitchPageMap(kernel_pagemap);
            
            uint64_t rflags;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&PROC_LIST_LOCK);
            if (curr_thread->parent) {
                if (curr_thread->next == curr_thread) proc->threads = nullptr;
                else {
                    if (proc->threads == curr_thread) proc->threads = curr_thread->next;
                    curr_thread->prev->next = curr_thread->next;
                    curr_thread->next->prev = curr_thread->prev;
                }
                curr_thread->parent = nullptr;
                curr_thread->next = curr_thread->prev = nullptr;
            }
            spinlock_unlock(&PROC_LIST_LOCK);
            
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&cpu->sched_lock);
            curr_thread->pagemap = kernel_pagemap;
            curr_thread->stack = 0;
            curr_thread->sig_stack = 0;
            curr_thread->tls_base = 0;
            __atomic_store_n(&curr_thread->state, THREAD_ZOMBIE, __ATOMIC_RELEASE);
            spinlock_unlock(&cpu->sched_lock);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

            asm volatile("sti");
            LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
            while(1) { asm volatile("hlt"); }
        }
        
        Serial::Writelnf("THREAD EXIT!!");
        VMM::SwitchPageMap(kernel_pagemap);

        uint64_t exit_rsp = (uint64_t)&cpu->exit_stack[4096];
        exit_rsp &= ~0xFULL; 
        
        TSS::SetRSP(cpu->id, 0, (void*)exit_rsp);
        cpu->kernel_stack = exit_rsp;
        asm volatile (
            "mov %0, %%rsp \n\t"
            "mov %1, %%rdi \n\t"
            "mov %2, %%rsi \n\t"
            "call *%3      \n\t"
            :
            : "r"(exit_rsp), "r"(proc), "r"(cpu), "r"(&FinalizeProcExit)
            : "memory", "rdi", "rsi"
            );
        while(1) { asm volatile("hlt"); }
    }

    void Exit(int32_t code) {
        asm volatile("cli"); LAPIC::StopTimer();
        proc_t *curr_proc = Schedule::this_proc();
        PROC_KILL(curr_proc, code);
    }
}