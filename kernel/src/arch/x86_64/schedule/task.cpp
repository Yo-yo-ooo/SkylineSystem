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

    void DeleteThread(cpu_t *cpu, thread_t *thread) {
        if (!cpu || !thread) return;

        // 1. 从运行队列和定时器轮盘移除
        spinlock_lock(&cpu->sched_lock);
        Schedule::Internal::RemoveFromQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);

        // 如果定时器在别的CPU上，需去对应CPU移除
        if (thread->timer_bucket != nullptr) {
            cpu_t *timer_cpu = smp_cpu_list[thread->timer_cpu];
            if (timer_cpu && timer_cpu != cpu) {
                spinlock_lock(&timer_cpu->sched_lock);
                Schedule::Internal::TimerRemove(thread);
                spinlock_unlock(&timer_cpu->sched_lock);
            }
        }

        // 2. 从进程线程链表移除
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

        // 3. 释放资源
        FreeThreadResources(thread);
        kfree(thread);
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;
        
        // 获取当前正在运行的线程，绝不能销毁自己！
        thread_t *curr_thread = this_cpu()->current_thread;

        // 1. 收集所有线程指针（持锁快照）
        spinlock_lock(&PROC_LIST_LOCK);
        thread_t *threads_to_delete[256];
        int thread_count = 0;
        thread_t *thread = proc->threads;
        if (thread) {
            thread_t *start = thread;
            do {
                // 【关键修复】：跳过当前正在执行的线程
                if (thread != curr_thread && thread_count < 256)
                    threads_to_delete[thread_count++] = thread;
                thread = thread->next;
            } while (thread != start);
        }
        proc->threads = nullptr; // 先断开链接
        spinlock_unlock(&PROC_LIST_LOCK);

        // 2. 逐个删除（不包含当前线程）
        for (int i = 0; i < thread_count; i++) {
            cpu_t *cpu = smp_cpu_list[threads_to_delete[i]->cpu_num];
            Schedule::DeleteThread(cpu, threads_to_delete[i]); 
        }
        
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
        
        // 4. 收集子进程指针
        proc_t *children_to_delete[256];
        int child_count = 0;
        proc_t *child = proc->children;
        while (child && child_count < 256) {
            children_to_delete[child_count++] = child;
            child = child->sibling;
        }
        proc->children = nullptr;
        spinlock_unlock(&PROC_LIST_LOCK);
        
        // 5. 递归删除子进程
        for (int i = 0; i < child_count; i++) {
            DeleteProc(children_to_delete[i]); 
        }
        
        // 6. 销毁文件描述符表与页表
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

        // 1. 标记其他线程为 ZOMBIE
        spinlock_lock(&PROC_LIST_LOCK);
        thread_t *t = proc->threads;
        while (t) {
            thread_t *next = (t->next == t) ? nullptr : t->next;
            if (t != curr_thread) {
                cpu_t *t_cpu = smp_cpu_list[t->cpu_num];
                if (t_cpu) {
                    spinlock_lock(&t_cpu->sched_lock);
                    t->state = THREAD_ZOMBIE;
                    spinlock_unlock(&t_cpu->sched_lock);
                    if (t->cpu_num != cpu->id) {
                        LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC + 1);
                    }
                }
            }
            t = next;
        }
        spinlock_unlock(&PROC_LIST_LOCK);

        // 2. 自旋等待其他线程全部被各自 CPU 丢弃
        bool all_stopped = false;
        while (!all_stopped) {
            all_stopped = true;
            spinlock_lock(&PROC_LIST_LOCK);
            t = proc->threads;
            while (t) {
                thread_t *next = (t->next == t) ? nullptr : t->next;
                if (t != curr_thread) {
                    cpu_t *t_cpu = smp_cpu_list[t->cpu_num];
                    if (t_cpu) {
                        spinlock_lock(&t_cpu->sched_lock);
                        bool still_running = (t->state == THREAD_RUNNING || t_cpu->current_thread == t);
                        if (still_running) {
                            all_stopped = false;
                            t->state = THREAD_ZOMBIE;
                        }
                        spinlock_unlock(&t_cpu->sched_lock);
                        if (still_running && t_cpu->id != cpu->id) {
                            LAPIC::IPI(t_cpu->lapic_id, SCHED_VEC + 1);
                        }
                    }
                }
                t = next;
            }
            spinlock_unlock(&PROC_LIST_LOCK);
            if (!all_stopped) asm volatile("pause");
        }

        // 防止 DeleteProc 释放当前栈
        if (curr_thread) {
            curr_thread->kernel_stack = 0;
        }

        // 3. 销毁进程一切资源 (已修改为跳过当前线程)
        Schedule::DeleteProc(proc);
        
        // 【关键修复】：此时 curr_thread 已经成了孤儿，它的 parent 已经被 kfree 了！
        // 必须将 parent 设为 nullptr，防止后续调度器解引用野指针
        curr_thread->parent = nullptr; 
        
        cpu->current_thread = nullptr;
        kinfoln("Delete PROC %d", pid);
        
        // 4. 触发调度
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