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
            kerrorln("Warning: Thread stuck in TRANSFER state for too long!");
            break;
        }
        asm volatile("pause");
    }
}

// Isolated batch kill logic to handle cross-CPU timer, migration races, and double insertion safely.
static void kill_thread_batch(thread_t *target, cpu_t *self_cpu, bool &need_wait) {
    while (true) {
        uint32_t target_cpu_num = __atomic_load_n(&target->cpu_num, __ATOMIC_ACQUIRE);
        cpu_t *t_cpu = get_cpu(target_cpu_num);
        
        // Fallback to self_cpu if target CPU is invalid to prevent thread leak
        if (!t_cpu) {
            t_cpu = self_cpu;
            spinlock_lock(&t_cpu->sched_lock);
            if (__atomic_load_n(&target->state, __ATOMIC_ACQUIRE) != THREAD_ZOMBIE) {
                target->state = THREAD_ZOMBIE;
                target->pagemap = kernel_pagemap;
                target->zombie_next = t_cpu->zombie_list;
                t_cpu->zombie_list = target;
                t_cpu->zombie_count++;
            }
            spinlock_unlock(&t_cpu->sched_lock);
            return;
        }

        // Safely remove cross-CPU timer before locking the target CPU
        if (target->timer_bucket != nullptr && target->timer_cpu != target_cpu_num) {
            cpu_t *timer_cpu = smp_cpu_list[target->timer_cpu];
            if (timer_cpu) {
                uint64_t rflags;
                asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
                spinlock_lock(&timer_cpu->sched_lock);
                if (target->timer_bucket != nullptr && target->timer_cpu != target_cpu_num) {
                    Schedule::Internal::TimerRemove(target);
                }
                spinlock_unlock(&timer_cpu->sched_lock);
                asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
            }
        }

        spinlock_lock(&t_cpu->sched_lock);
        
        // Re-check state and CPU under lock to prevent migration race
        if (__atomic_load_n(&target->cpu_num, __ATOMIC_ACQUIRE) != target_cpu_num) {
            spinlock_unlock(&t_cpu->sched_lock);
            continue; 
        }
        if (__atomic_load_n(&target->state, __ATOMIC_ACQUIRE) == THREAD_TRANSFER) {
            spinlock_unlock(&t_cpu->sched_lock);
            wait_for_transfer(target);
            continue; 
        }

        // Skip if already marked ZOMBIE by re-entry path or Switch()
        if (__atomic_load_n(&target->state, __ATOMIC_ACQUIRE) == THREAD_ZOMBIE) {
            spinlock_unlock(&t_cpu->sched_lock);
            return; 
        }

        target->state = THREAD_ZOMBIE;
        target->pagemap = kernel_pagemap;

        if (target->on_rq) {
            Schedule::Internal::RemoveFromQueue(t_cpu, target);
        } else if (target->timer_bucket) {
            if (target->timer_cpu == target_cpu_num) {
                Schedule::Internal::TimerRemove(target);
            }
        }

        bool need_ipi = false;
        if (t_cpu->current_thread == target) {
            // Do not add to zombie list here to prevent double insertion.
            // The Switch() function on the target CPU will handle it.
            need_ipi = true;
            need_wait = true;
        } else {
            target->zombie_next = t_cpu->zombie_list;
            t_cpu->zombie_list = target;
            t_cpu->zombie_count++;
        }
        spinlock_unlock(&t_cpu->sched_lock);

        if (need_ipi && t_cpu != self_cpu) {
            LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC + 1);
        }
        return;
    }
}

namespace Schedule {
    void DeleteProc(proc_t *proc);

    void FreeThreadResources(thread_t *thread) {
        if (thread->timer_bucket != nullptr) {
            cpu_t *timer_cpu = smp_cpu_list[thread->timer_cpu];
            if (timer_cpu) {
                uint64_t rflags;
                asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
                spinlock_lock(&timer_cpu->sched_lock);
                Schedule::Internal::TimerRemove(thread);
                spinlock_unlock(&timer_cpu->sched_lock);
                asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
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

    // Unified core destruction logic to prevent code redundancy and race conditions
    static proc_t* DestroyProcCore(proc_t *proc) {
        uint64_t rflags;
        proc_t *to_delete_list = nullptr;

        // 1. Detach from parent and collect children under PROC_LIST_LOCK
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
            proc->sibling = nullptr; // Protected under lock
        }
        
        // 2. Atomically acquire destruction rights for children
        proc_t *prev = nullptr;
        proc_t *child = proc->children;
        while (child) {
            proc_t *next = child->sibling;
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
            child = next;
        }
        proc->children = nullptr;
        spinlock_unlock(&PROC_LIST_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        // 3. Remove from PID tree
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_delete(pid2proc_tree, proc->id, 8);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        // 4. Destroy FDMan and pagemap (if not already handled by FinalizeProcExit)
        if (proc->FDMan) { fd_manager_destroy(proc->FDMan); kfree(proc->FDMan); }
        if (proc->pagemap && proc->pagemap != kernel_pagemap) VMM::DestroyPM(proc->pagemap);
        proc->pagemap = nullptr;

        kfree(proc);
        return to_delete_list;
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;
        
        // Atomic check-and-set to ensure single destruction path
        if (__sync_lock_test_and_set(&proc->exiting, 1) != 0) return;

        SyncKillProcThreads(proc, nullptr);

        proc_t *to_delete_list = DestroyProcCore(proc);
        
        while (to_delete_list) {
            proc_t *curr_child = to_delete_list;
            to_delete_list = curr_child->sibling;
            curr_child->sibling = nullptr;
            DeleteProc(curr_child);
        }
    }

    static void FinalizeProcExit(proc_t *proc, cpu_t *cpu) {
        uint64_t pid = proc->id;
        thread_t *curr_thread = cpu->current_thread;
        
        SyncKillProcThreads(proc, curr_thread);

        // Detach self from proc->threads list
        uint64_t rflags;
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
        spinlock_unlock(&PROC_LIST_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        // Extract pagemap and switch CR3 before destroying it
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

        proc_t *to_delete_list = DestroyProcCore(proc);

        // Mark as ZOMBIE and let Switch() handle the zombie list insertion
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
        spinlock_lock(&cpu->sched_lock);
        curr_thread->state = THREAD_ZOMBIE;
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        kinfoln("Delete PROC %d", pid);

        asm volatile("sti");
        while (to_delete_list) {
            proc_t *curr_child = to_delete_list;
            to_delete_list = curr_child->sibling;
            curr_child->sibling = nullptr;
            DeleteProc(curr_child);
        }

        LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
        while(true) { asm volatile("hlt"); }
    }

    void PROC_KILL(proc_t *proc, int32_t exit_code){
        thread_t *curr_thread = Schedule::this_thread();
        cpu_t *cpu = this_cpu();
        curr_thread->exit_code = exit_code;
        
        if (__sync_lock_test_and_set(&proc->exiting, 1) != 0) {
            // Re-entry path for concurrent exit
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
            
            // Lock protected state modification to prevent race with Switch()
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

        // Ensure 16-byte stack alignment before function call to prevent SIMD #GP
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