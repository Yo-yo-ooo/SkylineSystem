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
            kfree(thread->heap);

        kinfoln("Thread Heap Freed!");
        if(thread->fx_area)
            VMM::Free(kernel_pagemap, thread->fx_area);
        kinfoln("Thread FX Area freeed!");
        if(thread->stack)
            VMM::Free(kernel_pagemap,thread->stack);
        kinfoln("Thread Stack freeed!");
        if(thread->sig_stack)
            VMM::Free(kernel_pagemap,thread->sig_stack);
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

    void Exit(int32_t code){
        LAPIC::StopTimer();
        Schedule::DeleteProc(Schedule::this_proc());
        this_cpu()->current_thread = nullptr;
        kinfoln("Delete PROC!");
        LAPIC::IPI(this_cpu()->id, SCHED_VEC + 1);
    }
}