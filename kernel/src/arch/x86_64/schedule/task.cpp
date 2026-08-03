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
        if (PIT::TimeSinceBootMS() - wait_start > 1000) { // 1s 超时兜底
            kerrorln("Warning: Thread stuck in TRANSFER state for too long!");
            break;
        }
        asm volatile("pause");
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

            // 1. 持有进程锁：收集线程并从链表中摘除，彻底打破死循环
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

            bool processed[64] = {false};
            bool need_wait[64] = {false};

            // 2. 按CPU分组处理调度队列和定时器队列
            for (int i = 0; i < count; i++) {
                if (processed[i]) continue;
                
                
                wait_for_transfer(batch[i]);

                uint32_t target_cpu_num = batch[i]->cpu_num;
                cpu_t *t_cpu = get_cpu(target_cpu_num);
                if (!t_cpu) { processed[i] = true; continue; }

                bool need_ipi = false;
                uint64_t rflags2;
                asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags2) :: "memory");
                spinlock_lock(&t_cpu->sched_lock);

                for (int j = i; j < count; j++) {
                    if (processed[j]) continue;
                    if (batch[j]->cpu_num == target_cpu_num) {
                        thread_t *target = batch[j];
                        target->state = THREAD_ZOMBIE;
                        target->pagemap = kernel_pagemap; // 防止被 FreeThreadResources 访问已销毁的 pagemap

                        // 从运行队列移除
                        if (target->on_rq) {
                            Schedule::Internal::RemoveFromQueue(t_cpu, target);
                        }
                        // 从定时器队列移除
                        else if (target->timer_bucket) {
                            Schedule::Internal::TimerRemove(target);
                        }

                    
                        target->zombie_next = t_cpu->zombie_list;
                        t_cpu->zombie_list = target;
                        t_cpu->zombie_count++;

                        // 如果线程正在CPU上运行，需要触发调度让它下来
                        if (t_cpu->current_thread == target) {
                            need_ipi = true;
                            need_wait[j] = true;
                        }

                        processed[j] = true;
                    }
                }

                spinlock_unlock(&t_cpu->sched_lock);
                asm volatile("push %0\n\tpopfq" :: "r"(rflags2) : "memory");

                if (need_ipi && t_cpu != self_cpu) {
                    LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC + 1);
                }
            }

            // 3. 仅等待正在运行的线程离开CPU
            for (int i = 0; i < count; i++) {
                if (need_wait[i]) {
                    Schedule::WaitForThreadOffCpu(batch[i]);
                }
            }
        }
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;
        if (proc->parent) {
            uint64_t rflags;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&PROC_LIST_LOCK);
            if (proc->parent->children == proc) proc->parent->children = proc->sibling;
            else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) sibling = sibling->sibling;
                if (sibling) sibling->sibling = proc->sibling;
            }
            proc->parent = nullptr;
            spinlock_unlock(&PROC_LIST_LOCK);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        }
        
    
        proc_t *to_delete_list = proc;
        proc->sibling = nullptr;
        while (to_delete_list) {
            proc_t *curr = to_delete_list;
            to_delete_list = curr->sibling;
            SyncKillProcThreads(curr, nullptr);

            uint64_t rflags;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags) :: "memory");
            spinlock_lock(&PROC_LIST_LOCK);
            proc_t *child = curr->children;
            while (child) {
                proc_t *next_child = child->sibling;
                child->parent = nullptr; child->sibling = to_delete_list;
                to_delete_list = child; child = next_child;
            }
            curr->children = nullptr;
            spinlock_unlock(&PROC_LIST_LOCK);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

            if (curr->FDMan) { fd_manager_destroy(curr->FDMan); kfree(curr->FDMan); }
            if (curr->pagemap && curr->pagemap != kernel_pagemap) VMM::DestroyPM(curr->pagemap);

            uint64_t rflags2;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags2) :: "memory");
            spinlock_lock(&PID2PROC_TREE_LOCK);
            art_delete(pid2proc_tree, curr->id, 8);
            spinlock_unlock(&PID2PROC_TREE_LOCK);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags2) : "memory");
            kfree(curr);
        }
    }

    static void FinalizeProcExit(proc_t *proc, cpu_t *cpu) {
        uint64_t pid = proc->id;
        thread_t *curr_thread = cpu->current_thread;
        
        SyncKillProcThreads(proc, curr_thread);

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

        if (proc->parent) {
            uint64_t rflags2;
            asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags2) :: "memory");
            spinlock_lock(&PROC_LIST_LOCK);
            if (proc->parent->children == proc) proc->parent->children = proc->sibling;
            else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) sibling = sibling->sibling;
                if (sibling) sibling->sibling = proc->sibling;
            }
            proc->parent = nullptr;
            spinlock_unlock(&PROC_LIST_LOCK);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags2) : "memory");
        }

        uint64_t rflags3;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags3) :: "memory");
        spinlock_lock(&PROC_LIST_LOCK);
        proc_t *to_delete_list = proc->children;
        proc->children = nullptr;
        proc_t *temp = to_delete_list;
        while (temp) { temp->parent = nullptr; temp = temp->sibling; }
        spinlock_unlock(&PROC_LIST_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags3) : "memory");

        if (proc->FDMan) { fd_manager_destroy(proc->FDMan); kfree(proc->FDMan); }
        
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
        curr_thread->zombie_next = cpu->zombie_list;
        cpu->zombie_list = curr_thread;
        cpu->zombie_count++;
        cpu->current_thread = nullptr;
        spinlock_unlock(&cpu->sched_lock);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

        uint64_t rflags4;
        asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rflags4) :: "memory");
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_delete(pid2proc_tree, proc->id, 8);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        asm volatile("push %0\n\tpopfq" :: "r"(rflags4) : "memory");
        kfree(proc);

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
            
            VMM::SwitchPageMap(kernel_pagemap);
            
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
            
            spinlock_lock(&cpu->sched_lock);
            curr_thread->state = THREAD_ZOMBIE;
            curr_thread->pagemap = kernel_pagemap;
            curr_thread->stack = 0;
            curr_thread->sig_stack = 0;
            curr_thread->tls_base = 0;
            curr_thread->zombie_next = cpu->zombie_list;
            cpu->zombie_list = curr_thread;
            cpu->zombie_count++;
            cpu->current_thread = nullptr;
            spinlock_unlock(&cpu->sched_lock);
            asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");

            asm volatile("sti");
            LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
            while(1) { asm volatile("hlt"); }
        }
        
        Serial::Writelnf("THREAD EXIT!!");
        VMM::SwitchPageMap(kernel_pagemap);

        uint64_t exit_rsp = (uint64_t)&cpu->exit_stack[4096];
        TSS::SetRSP(cpu->id, 0, (void*)exit_rsp);
        cpu->kernel_stack = exit_rsp;
        asm volatile (
            "mov %0, %%rsp \n\t"
            "mov %1, %%rdi \n\t"
            "mov %2, %%rsi \n\t"
            "call *%3      \n\t"
            :
            : "r"((uint64_t)exit_rsp), "r"(proc), "r"(cpu), "r"(&FinalizeProcExit)
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