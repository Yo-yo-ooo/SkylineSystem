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
#include <atomic/atomic.h>

extern art_tree *pid2proc_tree;
extern spinlock_t PID2PROC_TREE_LOCK;
extern uint64_t sched_pid;
extern uint64_t sched_tid;

extern uint64_t elf_load(uint8_t *data, pagemap_t *pagemap, 
                  uint64_t *tls_offset = nullptr, 
                  uint64_t *tls_memsz = nullptr, 
                  uint64_t *tls_filesz = nullptr, 
                  uint64_t *tls_align = nullptr);

extern cpu_t *get_lw_cpu(cpu_t *ref_cpu = nullptr);
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

    extern void sched_fork_fds(fd_manager_t* parent_mgr, fd_manager_t* child_mgr);

    proc_t *ForkProcess() {
        proc_t *parent = this_proc();
        if (!parent) return nullptr;
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        if (!proc) return nullptr;
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid,1,ATOMIC_RELAXED);
        proc->parent = parent;
        if (!parent->children) parent->children = proc;
        else {
            proc_t *last = parent->children;
            while (last->sibling) last = last->sibling;
            last->sibling = proc;
        }
        proc->pagemap = VMM::Fork(parent->pagemap);
        
        // 必须重新初始化子进程的 FD 管理器，绝不能 memcpy！
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        fd_manager_init(proc->FDMan);
        sched_fork_fds(parent->FDMan, proc->FDMan);
        
        proc->fd_count = parent->fd_count;
        return proc;
    }

    thread_t* this_thread() { cpu_t* cpu = this_cpu(); return cpu ? cpu->current_thread : nullptr; }
    proc_t *this_proc() { thread_t* t = this_thread(); return t ? t->parent : nullptr; }
    
    void Yield() {
        LAPIC::StopTimer();
        asm volatile("int %0" :: "i"(SCHED_VEC + 1));
    }

    void PAUSE() { LAPIC::StopTimer(); }
    void Resume() {
        cpu_t* cpu = this_cpu();
        if (cpu) LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1); 
    }

    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int32_t priority, const char *Path, int32_t argc, char *argv[], char *envp[]) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        thread->id = atomic_add_fetch_8(&sched_tid,1,ATOMIC_RELAXED);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->pagemap = parent->pagemap;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);
        thread->held_resource_id = -1;
        thread->requested_resource_id = -1;
        thread->original_priority = -1;
        Schedule::Internal::ProcessAddThread(parent, thread);

        __hmap_s_mp *MP = GetMount(Path);
        if(!MP) { kerrorln("Cannot Find Mount Point!!!"); kfree(thread); return nullptr; }
        void *FileDesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
        if (!FileDesc) { kfree(thread); return nullptr; }
        _memset(FileDesc, 0, MP->FSOPS->SIZEOF_FILE_DESC);
        if(MP->FSOPS->open(FileDesc, Path, O_RDONLY) != 0) { kfree(FileDesc); kfree(thread); return nullptr; }
        uint64_t FSize = MP->FSOPS->fsize(FileDesc);
        
        // 优化：大文件用 VMM 按页分配，小文件用 Slab，防止 kmalloc 越界
        void *buffer = nullptr;
        if (FSize <= 16384) {
            buffer = kmalloc(FSize);
        } else {
            buffer = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(FSize, PAGE_SIZE), false);
        }
        if (!buffer) { MP->FSOPS->close(FileDesc); kfree(FileDesc); kfree(thread); return nullptr; }
        
        MP->FSOPS->read(FileDesc, buffer, FSize, 0);
        MP->FSOPS->close(FileDesc);
        kfree(FileDesc);
        
        uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
        _memset(&thread->ctx, 0, sizeof(context_t));
        thread->ctx.rip = elf_load((uint8_t*)buffer, thread->pagemap, &tls_offset, &tls_memsz, &tls_filesz, &tls_align); 
        if (thread->ctx.rip == 0) { kerrorln("ELF load failed!"); 
            if (FSize <= 16384) kfree(buffer); else VMM::Free(kernel_pagemap, buffer); 
            kfree(thread); return nullptr; 
        }

        cpu_t *cpu = get_cpu(cpu_num);
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);

        uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
        thread->stack = thread_stack;
        thread->thread_stack = thread_stack + 8 * PAGE_SIZE;

        uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
        thread->sig_stack = sig_stack;

        thread->ctx.cs = 0x23;
        thread->ctx.ss = 0x1b;
        thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread->thread_stack;
        PrepareUserStack(thread, argc, argv, envp);
        thread->thread_stack = thread->ctx.rsp;

        if (tls_memsz > 0) {
            if (tls_align == 0) tls_align = 16;
            uint64_t total_tls_size = ALIGN_UP(tls_memsz, tls_align) + 8;
            uint64_t tls_pages = DIV_ROUND_UP(total_tls_size, PAGE_SIZE);
            uint64_t tls_mem = (uint64_t)VMM::Alloc(thread->pagemap, tls_pages, true);
            uint64_t tcb_base = tls_mem + ALIGN_UP(tls_memsz, tls_align);
            VMM::SwitchPageMap(thread->pagemap);
            __memcpy((void*)(tcb_base - ALIGN_UP(tls_memsz, tls_align)), (void*)((uint8_t*)buffer + tls_offset), tls_filesz);
            *(uint64_t*)tcb_base = tcb_base; 
            VMM::SwitchPageMap(kernel_pagemap); 
            thread->fs = tcb_base;
            thread->tls_base = tls_mem;
            thread->tls_pages = tls_pages;
        }

        if (FSize <= 16384) kfree(buffer); else VMM::Free(kernel_pagemap, buffer);
        thread->state = THREAD_RUNNING;

        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        return thread;
    }

    thread_t *ForkThread(proc_t *proc, thread_t *parent, void *frame) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));

        cpu_t *parent_cpu = get_cpu(parent->cpu_num);
        cpu_t *cpu = get_lw_cpu(parent_cpu);       

        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        __memcpy(thread->fx_area, parent->fx_area, cpu->XsaveSize);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        __memcpy((void*)kernel_stack, (void*)parent->kernel_stack, 4 * PAGE_SIZE);

        thread->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
        thread->cpu_num = cpu->id;
        thread->parent = proc;
        thread->IsForkThread = true;
        thread->pagemap = proc->pagemap;
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + 4 * PAGE_SIZE;
        thread->stack = parent->stack;
        thread->sig_stack = parent->sig_stack;
        thread->tls_base = parent->tls_base;
        thread->tls_pages = parent->tls_pages;
        
        thread->held_resource_id = -1;
        thread->requested_resource_id = -1;
        thread->original_priority = -1;

        Schedule::Internal::ProcessAddThread(proc, thread);
        __memcpy(&thread->ctx, frame, sizeof(context_t));
        thread->ctx.rsp = parent->thread_stack;
        thread->ctx.cs = 0x23;
        thread->ctx.ss = 0x1b;
        thread->ctx.rflags = ((syscall_frame_t*)frame)->r11;
        thread->ctx.rax = 0;
        thread->ctx.rip = ((syscall_frame_t*)frame)->rcx;
        thread->thread_stack = parent->thread_stack;
        thread->fs = rdmsr(FS_BASE);
        thread->state = THREAD_RUNNING;

        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        return thread;
    }
}