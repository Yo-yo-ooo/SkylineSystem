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

extern art_tree *pid2proc_tree;
extern spinlock_t PID2PROC_TREE_LOCK;
extern spinlock_t PROC_LIST_LOCK;

namespace Schedule {
    
    // 前向声明
    void DeleteProc(proc_t *proc);
    
    void FreeThreadResources(thread_t *thread) {
        if (thread->timer_bucket != nullptr) {
            cpu_t *timer_cpu = smp_cpu_list[thread->timer_cpu];
            if (timer_cpu) {
                spinlock_lock(&timer_cpu->sched_lock);
                Schedule::Internal::TimerRemove(thread); 
                spinlock_unlock(&timer_cpu->sched_lock);
            }
        }

        cpu_t *cpu = get_cpu(thread->cpu_num);
        if (thread->fx_area) VMM::Free(kernel_pagemap, thread->fx_area);
        if (thread->kernel_stack) VMM::Free(kernel_pagemap, thread->kernel_stack);

        if (!thread->IsForkThread) {
            if (thread->pagemap != kernel_pagemap) {
                if (thread->stack && thread->stack != thread->kernel_stack) 
                    VMM::Free(thread->pagemap, thread->stack);
                if (thread->sig_stack) 
                    VMM::Free(thread->pagemap, thread->sig_stack);
                if (thread->tls_base) 
                    VMM::Free(thread->pagemap, thread->tls_base);
            }
        }
    }

    static void SyncKillProcThreads(proc_t *proc, thread_t *except_thread) {
        if (!proc) return;
        cpu_t *self_cpu = this_cpu();

        while (true) {
            thread_t *batch[64];
            int count = 0;
            
            spinlock_lock(&PROC_LIST_LOCK);
            thread_t *t = proc->threads;
            if (t) {
                thread_t *start = t;
                do {
                    if (t != except_thread) {
                        if (count < 64) {
                            batch[count++] = t;
                            t = t->next;
                        } else {
                            break;
                        }
                    } else {
                        t = t->next;
                    }
                } while (t != start);
            }
            spinlock_unlock(&PROC_LIST_LOCK);

            if (count == 0) break;

            bool processed[64] = {false};

            for (int i = 0; i < count; i++) {
                if (processed[i]) continue;

                uint32_t target_cpu_num = batch[i]->cpu_num;
                cpu_t *t_cpu = get_cpu(target_cpu_num);
                if (!t_cpu) continue;

                bool need_ipi = false;

                spinlock_lock(&t_cpu->sched_lock);
                
                for (int j = i; j < count; j++) {
                    if (processed[j]) continue;
                    if (batch[j]->cpu_num == target_cpu_num) {
                        thread_t *target = batch[j];
                        target->stack = 0;
                        target->sig_stack = 0;
                        target->tls_base = 0;
                        
                        target->state = THREAD_ZOMBIE;
                        processed[j] = true;
                        need_ipi = true;
                    }
                }
                
                spinlock_unlock(&t_cpu->sched_lock);

                if (need_ipi && t_cpu != self_cpu) {
                    LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC + 1);
                }
            }

            for (int i = 0; i < count; i++) {
                Schedule::WaitForThreadOffCpu(batch[i]);
                Schedule::KillThread(batch[i]);
            }
        }
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;

        // 1. 先从父进程链表移除，此时 sibling 仍为原始值
        if (proc->parent) {
            spinlock_lock(&PROC_LIST_LOCK);
            if (proc->parent->children == proc) {
                proc->parent->children = proc->sibling;
            } else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) {
                    sibling = sibling->sibling;
                }
                if (sibling) sibling->sibling = proc->sibling;
            }
            proc->parent = nullptr;
            spinlock_unlock(&PROC_LIST_LOCK);
        }

        // 父链表已脱离，现在可安全借用 sibling 构建迭代链表
        proc_t *to_delete_list = proc;
        proc->sibling = nullptr;

        while (to_delete_list) {
            proc_t *curr = to_delete_list;
            to_delete_list = curr->sibling;

            SyncKillProcThreads(curr, nullptr);

            spinlock_lock(&PROC_LIST_LOCK);
            proc_t *child = curr->children;
            while (child) {
                proc_t *next_child = child->sibling;
                child->parent = nullptr;
                child->sibling = to_delete_list;
                to_delete_list = child;
                child = next_child;
            }
            curr->children = nullptr;
            spinlock_unlock(&PROC_LIST_LOCK);

            if (curr->FDMan) {
                fd_manager_destroy(curr->FDMan);
                kfree(curr->FDMan);
            }
            if (curr->pagemap && curr->pagemap != kernel_pagemap) {
                VMM::DestroyPM(curr->pagemap);
            }

            spinlock_lock(&PID2PROC_TREE_LOCK);
            art_delete(pid2proc_tree, curr->id, 8);
            spinlock_unlock(&PID2PROC_TREE_LOCK);
            kfree(curr);
        }
    }

    static void FinalizeProcExit(proc_t *proc, cpu_t *cpu) {
        uint64_t pid = proc->id;
        thread_t *curr_thread = cpu->current_thread;

        SyncKillProcThreads(proc, curr_thread);

        spinlock_lock(&PROC_LIST_LOCK);
        if (curr_thread->next == curr_thread) proc->threads = nullptr;
        else {
            if (proc->threads == curr_thread) proc->threads = curr_thread->next;
            curr_thread->prev->next = curr_thread->next;
            curr_thread->next->prev = curr_thread->prev;
        }
        curr_thread->parent = nullptr;
        spinlock_unlock(&PROC_LIST_LOCK);

        if (proc->parent) {
            spinlock_lock(&PROC_LIST_LOCK);
            if (proc->parent->children == proc) {
                proc->parent->children = proc->sibling;
            } else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) {
                    sibling = sibling->sibling;
                }
                if (sibling) sibling->sibling = proc->sibling;
            }
            proc->parent = nullptr;
            spinlock_unlock(&PROC_LIST_LOCK);
        }

        spinlock_lock(&PROC_LIST_LOCK);
        proc_t *to_delete_list = proc->children;
        proc->children = nullptr;
        proc_t *temp = to_delete_list;
        while (temp) {
            temp->parent = nullptr;
            temp = temp->sibling;
        }
        spinlock_unlock(&PROC_LIST_LOCK);

        if (proc->FDMan) {
            fd_manager_destroy(proc->FDMan);
            kfree(proc->FDMan);
        }
        
        if (proc->pagemap && proc->pagemap != kernel_pagemap) {
            VMM::DestroyPM(proc->pagemap);
        }

        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_delete(pid2proc_tree, proc->id, 8);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        kfree(proc);

        curr_thread->pagemap = kernel_pagemap; 
        curr_thread->stack = 0;
        curr_thread->sig_stack = 0;
        curr_thread->tls_base = 0;
        
        FreeThreadResources(curr_thread);
        kfree(curr_thread);
        
        cpu->current_thread = nullptr;
        kinfoln("Delete PROC %d", pid);

        while (to_delete_list) {
            proc_t *curr_child = to_delete_list;
            to_delete_list = curr_child->sibling;
            curr_child->sibling = nullptr;
            DeleteProc(curr_child);
        }
        
        asm volatile("sti");
        LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
        while(true) { asm volatile("hlt"); }
    }

    void PROC_KILL(proc_t *proc, int32_t exit_code){
        thread_t *curr_thread = Schedule::this_thread();
        cpu_t *cpu = this_cpu();

        curr_thread->state = THREAD_ZOMBIE;
        curr_thread->exit_code = exit_code;

        if (__sync_lock_test_and_set(&proc->exiting, 1) != 0) {
            VMM::SwitchPageMap(kernel_pagemap);
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
        asm volatile("cli");
        LAPIC::StopTimer();
        proc_t *curr_proc = Schedule::this_proc();
        PROC_KILL(curr_proc, code);
    }
}