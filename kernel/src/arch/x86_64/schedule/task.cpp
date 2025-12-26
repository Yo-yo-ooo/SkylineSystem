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

        VMM::Free(kernel_pagemap, thread->fx_area);
        VMM::Free(kernel_pagemap,thread->kernel_stack);
        VMM::Free(kernel_pagemap,thread->stack);
        VMM::Free(kernel_pagemap,thread->sig_stack);
    }

    void DeleteThread(cpu_t *cpu, thread_t *thread) {
        // 1. 根据线程的优先级找到对应队列
        thread_queue_t *queue = &cpu->thread_queues[thread->priority];
    
        // 2. 处理队列中只有这一个线程的情况
        if (thread->list_next == thread) {
            queue->head = NULL;
            queue->current = NULL;  // 清空当前指针
        } else {
            // 3. 从循环链表中安全摘除该线程
            thread->list_prev->list_next = thread->list_next;
            thread->list_next->list_prev = thread->list_prev;
            
            // 4. 如果删除的是头节点，需要更新头指针
            if (queue->head == thread) {
                queue->head = thread->list_next;
            }
            // 5. 如果删除的是当前运行线程，需要更新当前指针
            // 注意：这里选择将current设为新的头节点，实际策略可能因调度需求而异
            if (queue->current == thread) {
                queue->current = queue->head;  // 或其他调度策略
            }
        }
        
        // 6. 可选：将线程的链表指针置空，避免悬空引用
        thread->list_next = thread->list_prev = NULL;
        
        FreeThreadResources(thread);

        // 7. 更新线程计数
        cpu->thread_count--;
    }

    void DeleteProc(proc_t *proc) {
        if (!proc) return;
        
        // 1. 终止进程中的所有线程
        thread_t *thread = proc->threads;
        while (thread) {
            thread_t *next_thread = thread->next;
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
        thread_t *thread = Schedule::this_thread();
        thread->state = THREAD_ZOMBIE;
        thread->exit_code = code;
        
        kinfoln("Do exit %d",code);
        // Wake up any threads waiting on this process
        proc_t *parent = Schedule::this_proc()->parent;
        if (!parent) {
            Schedule::Yield();
            return;
        }
        thread_t *child = parent->threads;
        do {
            child->sig_deliver |= 1 << 17;
            child->waiting_status = code | (thread->id << 32);
            child = child->next;
        } while (child != parent->threads);

        //Delete PROC!
        Schedule::DeleteProc(Schedule::this_proc());
        Schedule::Yield();
    }
}