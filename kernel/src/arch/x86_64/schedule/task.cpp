/*
* SPDX-License-Identifier: GPL-2.0-only
* File: task.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <arch/x86_64/allin.h>
#include <elf/elf.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>

namespace Schedule{
    void FreeThreadResources(thread_t *thread){
        if(thread->heap != nullptr)
            VMM::Free(thread->pagemap, thread->heap);
        kinfoln("Thread Heap Freed!");

        if(thread->fx_area)
            VMM::Free(kernel_pagemap, thread->fx_area);
        kinfoln("Thread FX Area freeed!");

        if(thread->stack)
            VMM::Free(kernel_pagemap, thread->stack);
        kinfoln("Thread Stack freeed!");

        if(thread->sig_stack)
            VMM::Free(kernel_pagemap, thread->sig_stack);
        kinfoln("Thread Signal Stack freeed!");
        
        kinfoln("Thread Res All freeed!");
    }

    void DeleteThread(cpu_t *cpu, thread_t *thread) {
        // 1. 根据线程的优先级找到对应队列
        thread_queue_t *queue = &cpu->thread_queues[thread->priority];
    
        // 2. 处理队列中只有这一个线程的情况
        if (thread->list_next == thread && thread->list_prev == thread) {
            // 队列中只有这一个线程
            if (queue->head == thread) {
                queue->head = NULL;
            }
            if (queue->current == thread) {
                queue->current = NULL;
            }
        } else {
            // 从循环双向链表中移除
            thread->list_prev->list_next = thread->list_next;
            thread->list_next->list_prev = thread->list_prev;
            
            // 更新队列头指针
            if (queue->head == thread) {
                queue->head = thread->list_next;
            }
            
            // 更新当前运行线程指针
            if (queue->current == thread) {
                queue->current = queue->head;
            }
        }
        
        FreeThreadResources(thread);

        thread->list_next = thread->list_prev = NULL;

        // 7. 更新线程计数
        cpu->thread_count--;
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;
        
        // 1. 终止进程中的所有线程
        thread_t *thread = proc->threads;
        while (thread) {
            thread_t *next_thread = thread->next != thread ? thread->next : NULL;
            cpu_t *cpu = smp_cpu_list[thread->cpu_num];
            Schedule::DeleteThread(cpu, thread); // Just Delete Thread(Mainly)
            thread->state = THREAD_ZOMBIE;
            thread = next_thread;
        }
        
        // 3. 从父进程的子进程链表中移除
        if (proc->parent) {
            // 如果这是父进程的第一个子进程
            if (proc->parent->children == proc) {
                proc->parent->children = proc->sibling;
            } else {
                // 在子进程链表中查找并移除
                proc_t *sibling = proc->parent->children;
                while (sibling && sibling->sibling != proc) {
                    sibling = sibling->sibling;
                }
                if (sibling) {
                    sibling->sibling = proc->sibling;
                }
            }
        }
        
        // 4. 处理子进程（可能需要终止或重新父级分配）
        if (proc->children) {
            // 策略1：终止所有子进程（递归）
            proc_t *child = proc->children;
            while (child) {
                proc_t *next_child = child->sibling;
                DeleteProc(child); // 递归删除
                child = next_child;
            }
        }
        
        VMM::DestroyPM(proc->pagemap);
    }

    // ====================================================================
    // 运行在 exit_stack 上的收尾函数
    // 此时旧的 thread->stack 和 proc->pagemap 都可以被安全炸毁
    // ====================================================================
    static void FinalizeProcExit(proc_t *proc, cpu_t *cpu) {
        // 彻底销毁进程的一切资源
        Schedule::DeleteProc(proc);
        
        cpu->current_thread = nullptr;
        kinfoln("Delete PROC!");
        
        // 触发调度中断，强制离开 exit_stack，加载新线程
        LAPIC::IPI(cpu->id, SCHED_VEC + 1);
        
        // 等待 IPI 响应，永远不会往下执行
        while(true) { asm volatile("hlt"); }
    }

    void Exit(int32_t code){
        asm volatile("cli");    // 必须关中断，保证切换过程绝对原子
        LAPIC::StopTimer();

        proc_t *curr_proc = Schedule::this_proc();
        thread_t *curr_thread = Schedule::this_thread();
        cpu_t *cpu = this_cpu();

        curr_thread->state = THREAD_ZOMBIE;
        curr_thread->exit_code = code;

        // 切入内核全局页表，脱离对 proc->pagemap 的依赖
        VMM::SwitchPageMap(kernel_pagemap);

        // 内联汇编强行切换栈指针，跳出当前 thread->stack
        // 前提：确保你的 cpu_t 结构体中已经定义了 exit_stack_top
        asm volatile (
            "mov %0, %%rsp \n\t"       // 将 RSP 指向 cpu->exit_stack_top
            "mov %1, %%rdi \n\t"       // rdi = 参数1: curr_proc
            "mov %2, %%rsi \n\t"       // rsi = 参数2: cpu
            "call *%3      \n\t"       // 间接调用 FinalizeProcExit
            :
            : "r"((uint64_t)&cpu->exit_stack[4096]), "r"(curr_proc), "r"(cpu), "r"(&FinalizeProcExit)
            : "memory", "rdi", "rsi"   // 声明污染了 RDI 和 RSI
        );

        // 如果代码执行到了这里，说明宇宙物理法则被打破了
        while(1) { asm volatile("hlt"); }
    }
}