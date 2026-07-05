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

// 统一全局变量，消除重复定义
static uint64_t sched_tid = 0;
static volatile uint64_t sched_pid = 0; 

extern uint64_t elf_load(uint8_t *data, pagemap_t *pagemap, 
                  uint64_t *tls_offset = nullptr, 
                  uint64_t *tls_memsz = nullptr, 
                  uint64_t *tls_filesz = nullptr, 
                  uint64_t *tls_align = nullptr);

// Idle 线程使用 hlt 休眠 CPU，避免死循环耗电
void sched_idle() {
    while (true) {
        Schedule::PAUSE();
        asm volatile("sti; hlt; cli" ::: "memory");
        Schedule::Resume();
    }
}

static cpu_t *get_lw_cpu() {
    cpu_t *lw_cpu = nullptr;
    for (int32_t i = 0; i <= smp_last_cpu; i++) {
        cpu_t *cpu = smp_cpu_list[i];
        if (cpu == nullptr) continue; 
        if (!lw_cpu) { lw_cpu = cpu; continue; }
        // 强制 volatile 读取，避免撕裂
        uint32_t current_count = *(volatile uint32_t*)&cpu->thread_count;
        uint32_t lowest_count  = *(volatile uint32_t*)&lw_cpu->thread_count;
        if (current_count < lowest_count) lw_cpu = cpu;
    }
    return lw_cpu ? lw_cpu : this_cpu();
}

namespace Schedule {
    namespace Internal {
        void ProcessAddThread(proc_t *parent, thread_t *thread) {
            if (!parent->threads) {
                parent->threads = thread;
                thread->next = thread;
                thread->prev = thread;
                return;
            }
            thread->next = parent->threads;
            thread->prev = parent->threads->prev;
            parent->threads->prev->next = thread;
            parent->threads->prev = thread;
        }

        void AddThread(cpu_t *cpu, thread_t *thread) {
            cpu->thread_count++;
            if (thread->priority > 0) {
                cpu->thread_count_lower++;
            }

            thread_queue_t *queue = &cpu->thread_queues[thread->priority];
            if (!queue->head) {
                thread->list_next = thread;
                thread->list_prev = thread;
                queue->head = thread;
                queue->current = thread;
                return;
            }
            thread->list_next = queue->head;
            thread->list_prev = queue->head->list_prev;
            queue->head->list_prev->list_next = thread;
            queue->head->list_prev = thread;
        }

        uint8_t Demote(cpu_t *cpu, thread_t *thread) {
            if (thread->priority == THREAD_QUEUE_CNT - 1) return 1;
            thread_queue_t *old_queue = &cpu->thread_queues[thread->priority];
            if (thread->priority == 0) cpu->thread_count_lower++;
            
            thread->priority++;
            thread_queue_t *new_queue = &cpu->thread_queues[thread->priority];
            if (thread->list_next == thread) {
                old_queue->head = nullptr;
                old_queue->current = nullptr;
            } else {
                if (old_queue->head == thread) old_queue->head = thread->list_next;
                if (old_queue->current == thread) old_queue->current = thread->list_next;
                thread->list_next->list_prev = thread->list_prev;
                thread->list_prev->list_next = thread->list_next;
            }
            --cpu->thread_count;
            Schedule::Internal::AddThread(cpu, thread);
            return 0;
        }

        void BoostPriority(cpu_t *cpu) {
            uint32_t approx_load = cpu->thread_count; 
            // EMA 四舍五入防截断
            cpu->load_ema = (cpu->load_ema * 7 + approx_load + 4) / 8;
            if (cpu->load_ema == 0) return; 

            uint64_t dynamic_threshold = cpu->boost_interval_base / cpu->load_ema;
            if (dynamic_threshold < 2) dynamic_threshold = 2;

            if (cpu->tick_count - cpu->last_boost_tick < dynamic_threshold) return;

            cpu->last_boost_tick = cpu->tick_count;
            uint32_t pending_in_lower = cpu->thread_count_lower;

            if (pending_in_lower == 0) {
                cpu->boost_interval_base += 50; 
            } else if (pending_in_lower > cpu->load_ema) {
                cpu->boost_interval_base /= 2;
            }

            if (cpu->boost_interval_base < 20) cpu->boost_interval_base = 20;
            if (cpu->boost_interval_base > 20000) cpu->boost_interval_base = 20000;

            thread_queue_t *top_queue = &cpu->thread_queues[0];
            
            for (uint32_t i = 1; i < THREAD_QUEUE_CNT; i++) {
                thread_queue_t *lower_queue = &cpu->thread_queues[i];
                if (!lower_queue->head) continue;

                thread_t *curr = lower_queue->head;
                do {
                    curr->priority = 0;
                    curr->preempt_count = 0;
                    curr->flags &= ~TFLAGS_PREEMPTED;
                    curr = curr->list_next;
                } while (curr != lower_queue->head);

                if (!top_queue->head) {
                    top_queue->head = lower_queue->head;
                    top_queue->current = lower_queue->head;
                } else {
                    thread_t *top_head = top_queue->head;
                    thread_t *top_tail = top_head->list_prev;
                    thread_t *low_head = lower_queue->head;
                    thread_t *low_tail = low_head->list_prev;

                    top_tail->list_next = low_head;
                    low_head->list_prev = top_tail;
                    low_tail->list_next = top_head;
                    top_head->list_prev = low_tail;
                }
                lower_queue->head = nullptr;
                lower_queue->current = nullptr;
            }
            cpu->thread_count_lower = 0;
        }

        thread_t *Pick(cpu_t *cpu) {
retry:
            for (uint32_t i = 0; i < THREAD_QUEUE_CNT; i++) {
                thread_queue_t *queue = &cpu->thread_queues[i];
                if (!queue->head) continue;
                if (!queue->current) queue->current = queue->head;

                thread_t *start = queue->current;
                thread_t *thread = start;
                bool found = false;
                
                do {
                    thread_t *next = thread->list_next;
                    if (thread->state == THREAD_RUNNING) {
                        if (!(thread->flags & TFLAGS_PREEMPTED)) {
                            thread->preempt_count = 0;
                            found = true;
                            break;
                        }
                        thread->preempt_count++;
                        if (thread->preempt_count >= SCHED_PREEMPTION_MAX) {
                            Schedule::Internal::Demote(cpu, thread);
                            thread->preempt_count = 0;
                            thread->flags &= ~TFLAGS_PREEMPTED;
                            goto retry; 
                        } else {
                            found = true;
                            break;
                        }
                    }
                    thread = next;
                } while (thread != start && queue->head != nullptr); 

                if (found) {
                    queue->current = thread->list_next;
                    thread->flags &= ~TFLAGS_PREEMPTED;
                    return thread;
                }
            }
            return nullptr;
        }

        void Switch(context_t *ctx) {
            // 移除全局 pushfq/cli，依赖 IDT 中断门特性与自旋锁保护
            LAPIC::StopTimer();
            
            cpu_t *cpu = this_cpu();
            if (!cpu || !cpu->thread_count || cpu->preempt_count > 0) {
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                return;
            }

            cpu->tick_count++;
            spinlock_lock(&cpu->sched_lock);
            
            Schedule::Internal::BoostPriority(cpu);
            
            thread_t *curr_thread = cpu->current_thread;
            if (curr_thread) {
                curr_thread->fs = rdmsr(FS_BASE);
                curr_thread->ctx = *ctx;
                // SIMD 指针判空保护
                if (curr_thread->fx_area) {
                    cpu->OverLoadableFuncs.StoreSIMDState(curr_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
                }
            }
            
            thread_t *next_thread = Schedule::Internal::Pick(cpu);
            if (!next_thread) {
                spinlock_unlock(&cpu->sched_lock);
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                return; 
            }
            
            cpu->current_thread = next_thread;
            *ctx = next_thread->ctx;
            TSS::SetRSP(cpu->id, 0, (void*)next_thread->kernel_rsp);
            cpu->kernel_stack = next_thread->kernel_rsp;
            
            if (!curr_thread || curr_thread->pagemap != next_thread->pagemap) {
                VMM::SwitchPageMap(next_thread->pagemap);
            }
            
            cpu->OverLoadableFuncs.WRFSBASE(next_thread->fs);
            if (next_thread->fx_area) {
                cpu->OverLoadableFuncs.LoadSIMDState(next_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
            }
            
            spinlock_unlock(&cpu->sched_lock);
            
            uint64_t final_ticks = cpu->thread_queues[next_thread->priority].quantum;
            LAPIC::Oneshot(SCHED_VEC, final_ticks);
            
            if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
        }

        void Preempt(context_t *ctx) {
            if (Schedule::this_thread()) {
                Schedule::this_thread()->flags |= TFLAGS_PREEMPTED;
                Schedule::this_thread()->preempt_count++;
            }
            Schedule::Internal::Switch(ctx);
        }
    }

    void Init() {
        // 使用宏定义，且不使用 IST 避免栈溢出隔离问题
        idt_install_irq(SCHED_VEC, (void*)Schedule::Internal::Preempt);
        idt_install_irq(SCHED_VEC + 1, (void*)Schedule::Internal::Switch);
        idt_set_ist(SCHED_VEC, 0);
        idt_set_ist(SCHED_VEC + 1, 0);
    }
    
    void Install() {
        for (uint32_t i = 0; i <= smp_last_cpu; i++) {
            cpu_t *cpu = smp_cpu_list[i];
            if (!cpu || cpu->id == smp_bsp_cpu) continue;
            proc_t *proc = Schedule::NewProcess(false);
            thread_t *thread = Schedule::NewKernelThread(proc, cpu->id, THREAD_QUEUE_CNT - 1, sched_idle);
        }
        atomic_store_8(
            (volatile uint8_t*)&PIT::TickHandle,
            (uint64_t)(uintptr_t)&PIT::Tick_,
            0
        );
    }

    proc_t *NewProcess(bool user) {
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        _memset(proc, 0, sizeof(proc_t)); // 清零防止信息泄露
        proc->id = atomic_add_fetch_8(&sched_pid, 1, 0); 
        proc->pagemap = (user ? VMM::NewPM() : kernel_pagemap);
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        fd_manager_init(proc->FDMan);
        proc->fd_count = 4;
        return proc;
    }

    void PrepareUserStack(thread_t *thread, int32_t argc, char *argv[], char *envp[]) {
        // 增加 kmalloc 失败判断
        char **kernel_argv = (char**)kmalloc(argc * 8);
        if (!kernel_argv) return;
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(argv[i]) + 1;
            kernel_argv[i] = (char*)kmalloc(size);
            if (!kernel_argv[i]) return;
            __memcpy(kernel_argv[i], argv[i], size);
        }
        int32_t envc = 0;
        while (envp[envc++]); envc -= 1;
        char **kernel_envp = (char**)kmalloc(envc * 8);
        if (!kernel_envp) return;
        for (int32_t i = 0; i < envc; i++) {
            int32_t size = strlen(envp[i]) + 1;
            kernel_envp[i] = (char*)kmalloc(size);
            if (!kernel_envp[i]) return;
            __memcpy(kernel_envp[i], envp[i], size);
        }
        uint64_t *thread_argv = (uint64_t*)kmalloc(argc * sizeof(uint64_t));
        if (!thread_argv) return;
        uint64_t stack_top = thread->ctx.rsp;
        uint64_t offset = 0;
        if ((argc + envc) % 2 == 0) offset = 8;
        pagemap_t *restore = VMM::SwitchPageMap(thread->pagemap);
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(kernel_argv[i]) + 1;
            offset += ALIGN_UP(size, 16); 
            thread_argv[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_argv[i], size);
        }
        uint64_t thread_envp[envc];
        for (int32_t i = 0; i < envc; i++) {
            int32_t size = strlen(kernel_envp[i]) + 1;
            offset += ALIGN_UP(size, 16);
            thread_envp[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_envp[i], size);
        }
        offset += 8;
        *(uint64_t*)(stack_top - offset) = 0; 
        for (int32_t i = envc - 1; i >= 0; i--) {
            offset += 8;
            *(uint64_t*)(stack_top - offset) = thread_envp[i];
        }
        offset += 8;
        *(uint64_t*)(stack_top - offset) = 0; 
        for (int32_t i = argc - 1; i >= 0; i--) {
            offset += 8;
            *(uint64_t*)(stack_top - offset) = thread_argv[i];
        }
        offset += 8;
        *(uint64_t*)(stack_top - offset) = argc;
        VMM::SwitchPageMap(restore);
        thread->ctx.rsp = stack_top - offset;
        for (int32_t i = 0; i < argc; i++) kfree(kernel_argv[i]);
        kfree(kernel_argv);
        for (int32_t i = 0; i < envc; i++) kfree(kernel_envp[i]);
        kfree(thread_argv);
        kfree(kernel_envp);
    }

    thread_t *NewKernelThread(proc_t *parent, uint32_t cpu_num, int32_t priority, void *entry) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        _memset(thread, 0, sizeof(thread_t)); // 清零
        thread->id = atomic_add_fetch_8(&sched_tid, 1, 0);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->IsForkThread = false;
        thread->pagemap = parent->pagemap;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);
        Schedule::Internal::ProcessAddThread(parent, thread);
        
        cpu_t *cpu = get_cpu(cpu_num);
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((cpu->XsaveSize), PAGE_SIZE), true);
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);

        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);
        thread->stack = kernel_stack;
        thread->ctx.rip = (uint64_t)entry;
        thread->ctx.cs = 0x08;
        thread->ctx.ss = 0x10;
        thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread->kernel_rsp;
        thread->thread_stack = thread->ctx.rsp;
        thread->state = THREAD_RUNNING;

        cpu_t *target_cpu = get_cpu(cpu_num);
        spinlock_lock(&target_cpu->sched_lock);
        target_cpu->has_runnable_thread = true;
        Schedule::Internal::AddThread(target_cpu, thread);
        spinlock_unlock(&target_cpu->sched_lock);
        return thread;
    }

    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int32_t priority, const char *Path, int32_t argc, char *argv[], char *envp[]) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        _memset(thread, 0, sizeof(thread_t));
        thread->id = atomic_add_fetch_8(&sched_tid, 1, 0);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->pagemap = parent->pagemap;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);
        Schedule::Internal::ProcessAddThread(parent, thread);

        __hmap_s_mp *MP = GetMount(Path);
        if(!MP) { kerrorln("Cannot Find Mount Point!!!"); kfree(thread); return nullptr; }
        uint64_t FileDescSize = MP->FSOPS->SIZEOF_FILE_DESC;
        void *FileDesc = kmalloc(FileDescSize);
        if (!FileDesc) { kerrorln("kmalloc FileDesc failed"); kfree(thread); return nullptr; }
        _memset(FileDesc, 0, FileDescSize);
        int32_t open_rc = MP->FSOPS->open(FileDesc, Path, O_RDONLY);
        if(open_rc != 0) { kerrorln("open file failed, ret = %d", open_rc); kfree(FileDesc); kfree(thread); return nullptr; }
        uint64_t FSize = MP->FSOPS->fsize(FileDesc);
        uint8_t *buffer = (uint8_t*)kmalloc(FSize);
        if (!buffer) { kfree(FileDesc); kerrorln("kmalloc buffer failed"); kfree(thread); return nullptr; }
        _memset(&thread->ctx, 0, sizeof(context_t));
        MP->FSOPS->read(FileDesc, buffer, FSize, 0);
        
        uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
        thread->ctx.rip = elf_load(buffer, thread->pagemap, &tls_offset, &tls_memsz, &tls_filesz, &tls_align); 
        
        // 完整的资源释放逻辑
        if (thread->ctx.rip == 0) {
            kerrorln("ELF load failed!");
            MP->FSOPS->close(FileDesc);
            kfree(FileDesc); kfree(buffer); kfree(thread);
            return nullptr; 
        }
        MP->FSOPS->close(FileDesc);
        kfree(FileDesc);

        cpu_t *cpu = get_cpu(cpu_num);
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((cpu->XsaveSize), PAGE_SIZE), true);
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);

        uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
        uint64_t thread_stack_top = thread_stack + 8 * PAGE_SIZE;
        thread->stack = thread_stack;

        uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
        thread->sig_stack = sig_stack;

        thread->ctx.cs = 0x23;
        thread->ctx.ss = 0x1b;
        thread->ctx.rflags = 0x202;
        thread->ctx.rsp = thread_stack_top;
        Schedule::PrepareUserStack(thread, argc, argv, envp);
        thread->thread_stack = thread->ctx.rsp;

        if (tls_memsz > 0) {
            if (tls_align == 0) tls_align = 16;
            uint64_t total_tls_size = ALIGN_UP(tls_memsz, tls_align) + 8;
            uint64_t tls_pages = DIV_ROUND_UP(total_tls_size, PAGE_SIZE);
            uint64_t tls_mem = (uint64_t)VMM::Alloc(thread->pagemap, tls_pages, true);
            uint64_t tcb_base = tls_mem + ALIGN_UP(tls_memsz, tls_align);
            uint64_t tls_data_start = tcb_base - ALIGN_UP(tls_memsz, tls_align);
            VMM::SwitchPageMap(thread->pagemap);
            __memcpy((void*)tls_data_start, (void*)(buffer + tls_offset), tls_filesz);
            *(uint64_t*)tcb_base = tcb_base; 
            VMM::SwitchPageMap(kernel_pagemap); 
            thread->fs = tcb_base;
        } else { thread->fs = 0; }

        kfree(buffer);
        thread->state = THREAD_RUNNING;

        cpu_t *target_cpu = get_cpu(cpu_num);
        spinlock_lock(&target_cpu->sched_lock);
        target_cpu->has_runnable_thread = true;
        Schedule::Internal::AddThread(target_cpu, thread);
        spinlock_unlock(&target_cpu->sched_lock);
        return thread;
    }

    thread_t *ForkThread(proc_t *proc, thread_t *parent, void *frame) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        _memset(thread, 0, sizeof(thread_t));
        
        cpu_t *cpu = get_lw_cpu();
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        __memcpy(thread->fx_area, parent->fx_area, cpu->XsaveSize);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        __memcpy((void*)kernel_stack, (void*)parent->kernel_stack, 4 * PAGE_SIZE);

        thread->id = atomic_add_fetch_8(&sched_tid, 1, 0);
        thread->cpu_num = cpu->id;
        thread->parent = proc;
        thread->IsForkThread = true;
        thread->pagemap = proc->pagemap; // VMM::Fork 已经处理了 COW
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + 4 * PAGE_SIZE;
        thread->stack = parent->stack; // 虚拟地址共享，物理页 COW 隔离
        thread->sig_stack = parent->sig_stack;

        Schedule::Internal::ProcessAddThread(proc, thread);
        typedef struct syscall_frame_t syscall_frame_t;
        syscall_frame_t *f = (syscall_frame_t*)frame;
        __memcpy(&thread->ctx, f, sizeof(context_t));
        thread->ctx.rsp = parent->thread_stack;
        thread->ctx.cs = 0x23;
        thread->ctx.ss = 0x1b;
        thread->ctx.rflags = f->r11;
        thread->ctx.rax = 0; 
        thread->ctx.rip = f->rcx; 
        thread->thread_stack = parent->thread_stack;
        thread->fs = rdmsr(FS_BASE);
        thread->state = THREAD_RUNNING;
        
        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Schedule::Internal::AddThread(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        return thread;
    }

    proc_t *ForkProcess() {
        proc_t *parent = Schedule::this_proc();
        if (!parent) return nullptr;
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid, 1, 0);
        proc->parent = parent;
        if (!parent->children) parent->children = proc;
        else {
            proc_t *last_sibling = parent->children;
            while (last_sibling->sibling != nullptr) last_sibling = last_sibling->sibling;
            last_sibling->sibling = proc;
        }
        proc->pagemap = VMM::Fork(parent->pagemap);
        __memcpy(proc->sig_handlers, parent->sig_handlers, 64 * sizeof(sigaction_t));
        // FDMan 深拷贝或引用计数，这里简单实现为独立分配
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        __memcpy(proc->FDMan, parent->FDMan, sizeof(fd_manager_t));
        proc->fd_count = parent->fd_count;
        return proc;
    }

    thread_t* this_thread() {
        cpu_t* cpu = this_cpu();
        if (!cpu) return nullptr;
        return cpu->current_thread;
    }

    proc_t *this_proc() {
        cpu_t* cpu = this_cpu();
        if (!cpu || !cpu->current_thread) return nullptr;
        return cpu->current_thread->parent;
    }
    
    void Yield() {
        LAPIC::StopTimer();
        if (Schedule::this_thread()) {
            Schedule::this_thread()->flags &= ~TFLAGS_PREEMPTED;
            Schedule::this_thread()->preempt_count = 0;
        }
        asm volatile("int %0" :: "i"(SCHED_VEC + 1)); // 使用宏定义
    }

    void PAUSE() { LAPIC::StopTimer(); }
    
    void Resume() {
        if (Schedule::this_thread()) {
            Schedule::this_thread()->flags &= ~TFLAGS_PREEMPTED;
            Schedule::this_thread()->preempt_count = 0;
        }
        // 使用 lapic_id 发送 IPI
        cpu_t* cpu = this_cpu();
        if (cpu) LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1); 
    }
}