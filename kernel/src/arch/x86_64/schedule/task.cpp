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

namespace Schedule {
    void FreeThreadResources(thread_t *thread) {
        debugpln("Thread Heap Freed!");

        cpu_t *cpu = get_cpu(thread->cpu_num);

        // 释放 SIMD 扩展状态保存区
        if (thread->fx_area) {
            VMM::Free(kernel_pagemap, thread->fx_area);
        }
        debugpln("Thread FX Area freed!");

        // 释放内核栈 (4页)
        if (thread->kernel_stack) {
            VMM::Free(kernel_pagemap, thread->kernel_stack);
        }
        debugpln("Thread Kernel Stack freed!");

        // 如果不是 Fork 产生的线程，才拥有用户态独占资源的所有权
        if (!thread->IsForkThread) {
            // 避免内核线程（user==false）或共享栈线程被误释放
            if (thread->pagemap != kernel_pagemap) {
                // 释放用户态栈 (8页)，且避免与 kernel_stack 冲突
                if (thread->stack && thread->stack != thread->kernel_stack) {
                    VMM::Free(thread->pagemap, thread->stack);
                }
                debugpln("Thread User Stack freed!");

                // 释放信号栈 (1页)
                if (thread->sig_stack) {
                    VMM::Free(thread->pagemap, thread->sig_stack);
                }
                debugpln("Thread Signal Stack freed!");

                // 释放 TLS 区域
                if (thread->tls_base) {
                    VMM::Free(thread->pagemap, thread->tls_base);
                }
                debugpln("Thread TLS freed!");
            }
        }

        // 从时间轮中安全摘除
        spinlock_lock(&cpu->sched_lock);
        Schedule::Internal::TimerRemove(thread); 
        spinlock_unlock(&cpu->sched_lock);

        debugpln("Thread Res All freed!");
    }

    void DeleteThread(cpu_t *cpu, thread_t *thread) {
        if (!cpu || !thread) return;

        // 从调度器的优先级队列中安全移除，统一由 RemoveFromQueue 维护计数器
        spinlock_lock(&cpu->sched_lock);
        Schedule::Internal::RemoveFromQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);

        // 从进程的线程双向链表中移除
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

        // 释放线程挂载的所有内存资源
        FreeThreadResources(thread);

        // 释放线程结构体本身，防止内存泄漏
        kfree(thread);
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;
        
        // 1. 终止并销毁进程中的所有线程
        thread_t *thread = proc->threads;
        while (thread) {
            thread_t *next_thread = (thread->next != thread) ? thread->next : nullptr;
            cpu_t *cpu = smp_cpu_list[thread->cpu_num];
            Schedule::DeleteThread(cpu, thread); 
            thread = next_thread;
        }
        
        // 2. 从父进程的子进程链表中移除
        if (proc->parent) {
            if (proc->parent->children == proc) {
                proc->parent->children = proc->sibling;
            } else {
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) {
                    sibling = sibling->sibling;
                }
                if (sibling) {
                    sibling->sibling = proc->sibling;
                }
            }
        }
        
        // 3. 处理子进程（递归删除，也可根据需要改为 init 进程接管）
        if (proc->children) {
            proc_t *child = proc->children;
            while (child) {
                proc_t *next_child = child->sibling;
                DeleteProc(child); 
                child = next_child;
            }
        }
        
        // 4. 销毁文件描述符表与页表
        if (proc->FDMan) {
            fd_manager_destroy(proc->FDMan);
            kfree(proc->FDMan);
        }
        
        if (proc->pagemap && proc->pagemap != kernel_pagemap) {
            VMM::DestroyPM(proc->pagemap);
        }

        
        spinlock_lock(&PID2PROC_TREE_LOCK);
        art_delete(pid2proc_tree,proc->id,8);
        spinlock_unlock(&PID2PROC_TREE_LOCK);
        kfree(proc);
    }

    static void FinalizeProcExit(proc_t *proc, cpu_t *cpu) {
        // 提前缓存 PID，因为 DeleteProc 后 proc 结构体将被物理释放
        uint64_t pid = proc->id;
        
        // 彻底销毁进程的一切资源
        Schedule::DeleteProc(proc);
        
        cpu->current_thread = nullptr;
        kinfoln("Delete PROC %d", pid);
        
        // 触发调度中断，强制离开 exit_stack，加载新线程
        LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);
        
        // 等待 IPI 响应，永远不会往下执行
        while(true) { asm volatile("hlt"); }
    }

    void PROC_KILL(proc_t *proc){
        thread_t *curr_thread = Schedule::this_thread();
        cpu_t *cpu = this_cpu();

        curr_thread->state = THREAD_ZOMBIE;
        curr_thread->exit_code = 0;

        // 切入内核全局页表，脱离对 proc->pagemap 的依赖
        VMM::SwitchPageMap(kernel_pagemap);

        // 内联汇编强行切换栈指针，跳出当前 thread->stack
        asm volatile (
            "mov %0, %%rsp \n\t"       // 将 RSP 指向 cpu->exit_stack 顶部
            "mov %1, %%rdi \n\t"       // rdi = 参数1: curr_proc
            "mov %2, %%rsi \n\t"       // rsi = 参数2: cpu
            "call *%3      \n\t"       // 间接调用 FinalizeProcExit
            :
            : "r"((uint64_t)&cpu->exit_stack[4096]), "r"(proc), "r"(cpu), "r"(&FinalizeProcExit)
            : "memory", "rdi", "rsi"   // 声明污染了 RDI 和 RSI
        );
               
        // 如果代码执行到了这里，说明宇宙物理法则被打破了
        while(1) { asm volatile("hlt"); }
    }

    void Exit(int32_t code) {
        asm volatile("cli");    // 必须关中断，保证切换过程绝对原子
        LAPIC::StopTimer();

        proc_t *curr_proc = Schedule::this_proc();
        PROC_KILL(curr_proc);
    }
}