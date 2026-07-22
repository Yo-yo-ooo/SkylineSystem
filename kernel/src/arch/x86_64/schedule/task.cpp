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
    
    void FreeThreadResources(thread_t *thread) {
        // 1. 先从定时器轮盘中移除（可能在不同CPU上）
        if (thread->timer_bucket != nullptr) {
            cpu_t *timer_cpu = smp_cpu_list[thread->timer_cpu];
            if (timer_cpu) {
                spinlock_lock(&timer_cpu->sched_lock);
                Schedule::Internal::TimerRemove(thread); 
                spinlock_unlock(&timer_cpu->sched_lock);
            }
        }

        // 2. 再释放其他资源
        cpu_t *cpu = get_cpu(thread->cpu_num);
        if (thread->fx_area) VMM::Free(kernel_pagemap, thread->fx_area);
        if (thread->kernel_stack) VMM::Free(kernel_pagemap, thread->kernel_stack);

        // 仅释放非 fork 线程且用户态指针非空的资源
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

    // 统一的进程内部资源销毁逻辑，消除代码冗余
    static void DestroyProcInternal(proc_t *proc) {
        if (!proc) return;

        // 1. 迭代式递归删除子进程，消除 256 数组上限与栈溢出风险
        while (true) {
            proc_t *child = nullptr;
            spinlock_lock(&PROC_LIST_LOCK);
            if (proc->children) {
                child = proc->children;
                proc->children = child->sibling;
                child->sibling = nullptr;
            }
            spinlock_unlock(&PROC_LIST_LOCK);
            
            if (!child) break;
            
            // 【关键修复】：必须调用完整的 DeleteProc，确保子进程的线程被同步杀死后再销毁资源
            DeleteProc(child);
        }

        // 2. 销毁文件描述符表与页表
        if (proc->FDMan) {
            fd_manager_destroy(proc->FDMan);
            kfree(proc->FDMan);
        }
        
        if (proc->pagemap && proc->pagemap != kernel_pagemap) {
            VMM::DestroyPM(proc->pagemap);
        }

        // 3. 从 PID 树移除并释放进程结构体
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_delete(pid2proc_tree, proc->id, 8);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        kfree(proc);
    }

    // 同步杀死进程内除指定线程外的所有线程
    static void SyncKillProcThreads(proc_t *proc, thread_t *except_thread) {
        if (!proc) return;
        cpu_t *self_cpu = this_cpu();

        while (true) {
            thread_t *target = nullptr;
            
            spinlock_lock(&PROC_LIST_LOCK);
            thread_t *t = proc->threads;
            if (t) {
                thread_t *start = t;
                do {
                    if (t != except_thread) {
                        target = t;
                        break;
                    }
                    t = t->next;
                } while (t != start);
            }

            if (target) {
                // 【关键修复 UAF】：提前清零用户态指针，防止后续 DestroyPM 销毁页表后，
                // 调度器异步回收僵尸线程时再次调用 VMM::Free 释放用户态内存导致 UAF。
                // 进程的页表销毁会自动回收所有用户态物理内存，此处跳过单独释放是安全的。
                target->stack = 0;
                target->sig_stack = 0;
                target->tls_base = 0;
            }
            spinlock_unlock(&PROC_LIST_LOCK);

            if (!target) break;

            cpu_t *t_cpu = get_cpu(target->cpu_num);
            if (t_cpu) {
                spinlock_lock(&t_cpu->sched_lock);
                target->state = THREAD_ZOMBIE;
                spinlock_unlock(&t_cpu->sched_lock);
                if (t_cpu != self_cpu) {
                    LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC + 1);
                }
            }

            // 等待目标线程下 CPU
            Schedule::WaitForThreadOffCpu(target);

            // 安全地杀死并移交调度器回收
            Schedule::KillThread(target);
        }
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;

        // 1. 同步杀死所有线程
        SyncKillProcThreads(proc, nullptr);

        // 2. 从父进程的子进程链表中移除
        spinlock_lock(&PROC_LIST_LOCK);
        if (proc->parent) {
            if (proc->parent->children == proc) {
                proc->parent->children = proc->sibling;
            } else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) {
                    sibling = sibling->sibling;
                }
                if (sibling) sibling->sibling = proc->sibling;
            }
        }
        spinlock_unlock(&PROC_LIST_LOCK);

        // 3. 统一销毁进程资源与子进程
        DestroyProcInternal(proc);
    }

    static void FinalizeProcExit(proc_t *proc, cpu_t *cpu) {
        uint64_t pid = proc->id;
        thread_t *curr_thread = cpu->current_thread;

        // 1. 同步杀死同进程的其他线程
        SyncKillProcThreads(proc, curr_thread);

        // 2. 此时所有其他线程均已安全退出并被回收，从进程链表摘除自己
        spinlock_lock(&PROC_LIST_LOCK);
        if (curr_thread->next == curr_thread) proc->threads = nullptr;
        else {
            if (proc->threads == curr_thread) proc->threads = curr_thread->next;
            curr_thread->prev->next = curr_thread->next;
            curr_thread->next->prev = curr_thread->prev;
        }
        curr_thread->parent = nullptr;
        spinlock_unlock(&PROC_LIST_LOCK);

        // 3. 从父进程的子进程链表中移除
        spinlock_lock(&PROC_LIST_LOCK);
        if (proc->parent) {
            if (proc->parent->children == proc) {
                proc->parent->children = proc->sibling;
            } else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) {
                    sibling = sibling->sibling;
                }
                if (sibling) sibling->sibling = proc->sibling;
            }
        }
        spinlock_unlock(&PROC_LIST_LOCK);

        // 4. 统一销毁进程资源（页表、FD、子进程等）
        DestroyProcInternal(proc);

        // 5. 页表已被销毁，清零当前线程用户态指针，防止野指针操作
        curr_thread->pagemap = kernel_pagemap; 
        curr_thread->stack = 0;
        curr_thread->sig_stack = 0;
        curr_thread->tls_base = 0;
        
        // 6. 在 exit_stack 上安全清理自己的内核态资源
        FreeThreadResources(curr_thread);
        kfree(curr_thread);
        
        cpu->current_thread = nullptr;
        kinfoln("Delete PROC %d", pid);
        
        // 7. 触发调度
        asm volatile("sti");
        LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
        while(true) { asm volatile("hlt"); }
    }

    void PROC_KILL(proc_t *proc, int32_t exit_code){
        thread_t *curr_thread = Schedule::this_thread();
        cpu_t *cpu = this_cpu();

        curr_thread->state = THREAD_ZOMBIE;
        curr_thread->exit_code = exit_code;

        // 原子检查退出标志，防止多核并发销毁同一个进程
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