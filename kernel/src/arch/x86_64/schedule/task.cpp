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

        spinlock_lock(&cpu->sched_lock);
        Schedule::Internal::TimerRemove(thread); 
        spinlock_unlock(&cpu->sched_lock);
    }

    void DeleteThread(cpu_t *cpu, thread_t *thread) {
        if (!cpu || !thread) return;

        spinlock_lock(&cpu->sched_lock);
        Schedule::Internal::RemoveFromQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);

        spinlock_lock(&PROC_LIST_LOCK);
        proc_t *parent = thread->parent;
        if (parent && parent->threads) {
            if (thread->next == thread) {
                parent->threads = nullptr;
            } else {
                if (parent->threads == thread) parent->threads = thread->next;
                thread->prev->next = thread->next;
                thread->next->prev = thread->prev;
            }
        }
        spinlock_unlock(&PROC_LIST_LOCK);

        FreeThreadResources(thread);
        kfree(thread);
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;
        
        // 1. 终止并销毁进程中的所有线程
        spinlock_lock(&PROC_LIST_LOCK);
        thread_t *thread = proc->threads;
        while (thread) {
            thread_t *next_thread = (thread->next != thread) ? thread->next : nullptr;
            cpu_t *cpu = smp_cpu_list[thread->cpu_num];
            spinlock_unlock(&PROC_LIST_LOCK);
            
            Schedule::DeleteThread(cpu, thread); 
            
            spinlock_lock(&PROC_LIST_LOCK);
            thread = next_thread;
        }
        spinlock_unlock(&PROC_LIST_LOCK);
        
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
        
        // 3. 将子进程 reparent 给 init（此处简化为直接挂到最顶层，或交由父进程接管）
        // 注：理想情况下应过继给 init 进程，这里保持原有的递归删除逻辑
        if (proc->children) {
            proc_t *child = proc->children;
            while (child) {
                proc_t *next_child = child->sibling;
                spinlock_unlock(&PROC_LIST_LOCK);
                DeleteProc(child); 
                spinlock_lock(&PROC_LIST_LOCK);
                child = next_child;
            }
        }
        spinlock_unlock(&PROC_LIST_LOCK);
        
        // 4. 销毁文件描述符表与页表
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
    }

    static void FinalizeProcExit(proc_t *proc, cpu_t *cpu) {
        uint64_t pid = proc->id;
        thread_t *curr_thread = cpu->current_thread;

        // 1. 标记所有同进程其他线程为 ZOMBIE，并发送 IPI 强制其下线
        spinlock_lock(&PROC_LIST_LOCK);
        thread_t *t = proc->threads;
        while (t) {
            if (t != curr_thread) {
                t->state = THREAD_ZOMBIE;
                if (t->cpu_num != cpu->id) {
                    cpu_t *t_cpu = smp_cpu_list[t->cpu_num];
                    if (t_cpu) LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC + 1);
                }
            }
            t = (t->next == t) ? nullptr : t->next;
        }
        spinlock_unlock(&PROC_LIST_LOCK);

        // 2. 自旋等待其他线程全部被各自 CPU 丢弃并停止使用该进程的 pagemap
        bool all_stopped = false;
        while (!all_stopped) {
            all_stopped = true;
            spinlock_lock(&PROC_LIST_LOCK);
            t = proc->threads;
            while (t) {
                if (t != curr_thread) {
                    cpu_t *t_cpu = smp_cpu_list[t->cpu_num];
                    // 如果它还在运行，或者还被挂在某 CPU 上
                    if (t->state == THREAD_RUNNING || (t_cpu && t_cpu->current_thread == t)) {
                        all_stopped = false;
                        t->state = THREAD_ZOMBIE;
                        if (t_cpu && t_cpu->id != cpu->id) {
                            LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC + 1);
                        }
                    }
                }
                t = (t->next == t) ? nullptr : t->next;
            }
            spinlock_unlock(&PROC_LIST_LOCK);
            if (!all_stopped) asm volatile("pause");
        }

        // 3. 此时除了当前线程，其他线程均已安全退出运行，可以销毁进程一切资源
        Schedule::DeleteProc(proc);
        
        cpu->current_thread = nullptr;
        kinfoln("Delete PROC %d", pid);
        
        // 4. 开中断并触发自陷调度，彻底离开 exit_stack
        asm volatile("sti");
        LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
        
        while(true) { asm volatile("hlt"); }
    }

    void PROC_KILL(proc_t *proc, int32_t exit_code){
        thread_t *curr_thread = Schedule::this_thread();
        cpu_t *cpu = this_cpu();

        curr_thread->state = THREAD_ZOMBIE;
        curr_thread->exit_code = exit_code;

        VMM::SwitchPageMap(kernel_pagemap);

        asm volatile (
            "mov %0, %%rsp \n\t"
            "mov %1, %%rdi \n\t"
            "mov %2, %%rsi \n\t"
            "call *%3      \n\t"
            :
            : "r"((uint64_t)&cpu->exit_stack[4096]), "r"(proc), "r"(cpu), "r"(&FinalizeProcExit)
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