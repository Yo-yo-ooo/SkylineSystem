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

static uint64_t sched_tid = 0;
static uint64_t sched_pid = 0;

// 老化阈值：优先级越低，容忍的等待 tick 越长
#define AGING_THRESHOLD_BASE 50

extern uint64_t elf_load(uint8_t *data, pagemap_t *pagemap, 
                  uint64_t *tls_offset = nullptr, 
                  uint64_t *tls_memsz = nullptr, 
                  uint64_t *tls_filesz = nullptr, 
                  uint64_t *tls_align = nullptr);

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
        uint32_t current_count = atomic_load_4(&cpu->thread_count,1);
        uint32_t lowest_count = atomic_load_4(&lw_cpu->thread_count,1);
        if (current_count < lowest_count) lw_cpu = cpu;
    }
    return lw_cpu ? lw_cpu : this_cpu();
}

namespace Schedule {
    namespace Internal {
        // 统一的队列移除，自动维护计数器
        void RemoveFromQueue(cpu_t *cpu, thread_t *thread) {
            thread_queue_t *q = &cpu->thread_queues[thread->priority];
            if (thread->list_next == thread) {
                q->head = nullptr;
                q->current = nullptr;
            } else {
                if (q->head == thread) q->head = thread->list_next;
                if (q->current == thread) q->current = thread->list_next;
                thread->list_next->list_prev = thread->list_prev;
                thread->list_prev->list_next = thread->list_next;
            }
            thread->list_next = thread->list_prev = nullptr;
            cpu->thread_count--;
            if (thread->priority > 0) cpu->thread_count_lower--;
        }

        // 统一的队列插入，自动维护计数器
        void InsertToQueue(cpu_t *cpu, thread_t *thread) {
            thread_queue_t *q = &cpu->thread_queues[thread->priority];
            cpu->thread_count++;
            if (thread->priority > 0) cpu->thread_count_lower++;

            if (!q->head) {
                thread->list_next = thread;
                thread->list_prev = thread;
                q->head = thread;
                q->current = thread;
            } else {
                thread->list_next = q->head;
                thread->list_prev = q->head->list_prev;
                q->head->list_prev->list_next = thread;
                q->head->list_prev = thread;
            }
        }

        void Demote(cpu_t *cpu, thread_t *thread) {
            if (thread->priority == THREAD_QUEUE_CNT - 1) return;
            RemoveFromQueue(cpu, thread);
            thread->priority++;
            thread->wait_ticks = 0;
            thread->preempt_count = 0;
            InsertToQueue(cpu, thread);
        }

        // 平滑提升优先级，防止 Boost 风暴
        void Promote(cpu_t *cpu, thread_t *thread) {
            if (thread->priority == 0) return;
            RemoveFromQueue(cpu, thread);
            thread->priority--;
            thread->wait_ticks = 0;
            InsertToQueue(cpu, thread);
        }

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

        thread_t *Pick(cpu_t *cpu) {
            for (uint32_t i = 1; i < THREAD_QUEUE_CNT; i++) {
                thread_queue_t *q = &cpu->thread_queues[i];
                if (!q->head) continue;
                
                thread_t *start = q->head;
                thread_t *curr = start;
                
                while (true) {
                    // 必须先保存 next，因为 Promote 会将 curr 移出当前队列并破坏其指针
                    thread_t *next = curr->list_next;
                    bool removed = false;
                    
                    if (curr->state == THREAD_RUNNING) {
                        curr->wait_ticks++;
                        // 统计等待 tick
                        cpu->sched_stats.total_wait_ticks++; 
                        
                        if (curr->wait_ticks > (AGING_THRESHOLD_BASE * (i + 1))) {
                            Promote(cpu, curr);
                            cpu->sched_stats.aging_promotions++; // 统计升权次数
                            removed = true;
                        }
                    }
                    
                    if (removed) {
                        // curr 被移除，如果它是唯一节点，队列已空
                        if (next == curr) break; 
                        // 如果起始节点被移除，更新 start 指针为新的合法节点
                        if (curr == start) start = next; 
                        // 不判断 next == start，直接继续处理 next
                        curr = next;
                    } else {
                        // curr 未被移除，如果转了一圈回到了 start，则结束本队列遍历
                        if (next == start) break;
                        curr = next;
                    }
                }
            }

            // 2. 严格按优先级轮转选取
            for (uint32_t i = 0; i < THREAD_QUEUE_CNT; i++) {
                thread_queue_t *q = &cpu->thread_queues[i];
                if (!q->head) continue;
                if (!q->current) q->current = q->head;

                thread_t *start = q->current;
                thread_t *t = start;
                do {
                    if (t->state == THREAD_RUNNING) {
                        q->current = t->list_next;
                        t->wait_ticks = 0;
                        t->preempt_count = 0;
                        return t;
                    }
                    t = t->list_next;
                } while (t != start);
            }
            return nullptr;
        }

        void Switch(context_t *ctx) {
            LAPIC::StopTimer();
            cpu_t *cpu = this_cpu();
            if (!cpu || cpu->preempt_count > 0 || cpu->thread_count == 0) {
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                return;
            }

            cpu->tick_count++;
            spinlock_lock(&cpu->sched_lock);
            
            thread_t *curr_thread = cpu->current_thread;
            if (curr_thread) {
                curr_thread->fs = rdmsr(FS_BASE);
                curr_thread->ctx = *ctx;
                if (curr_thread->fx_area) {
                    cpu->OverLoadableFuncs.StoreSIMDState(curr_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
                }
                
                // 时间片耗尽降级
                if (curr_thread->state == THREAD_RUNNING) {
                    curr_thread->preempt_count++;
                    if (curr_thread->preempt_count >= SCHED_PREEMPTION_MAX) {
                        Demote(cpu, curr_thread);
                    }
                }
            }
            
            thread_t *next_thread = Pick(cpu);
            if (!next_thread || next_thread == curr_thread) {
                spinlock_unlock(&cpu->sched_lock);
                if (next_thread == curr_thread) {
                    LAPIC::Oneshot(SCHED_VEC, cpu->thread_queues[curr_thread->priority].quantum);
                }
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                return; 
            }
            
            cpu->current_thread = next_thread;
            // 统计上下文切换次数
            cpu->sched_stats.context_switches++; 
            
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
            
            LAPIC::Oneshot(SCHED_VEC, cpu->thread_queues[next_thread->priority].quantum);
            if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
        }

        void Preempt(context_t *ctx) {
            Switch(ctx);
        }
    }

    void Init() {
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
            Schedule::NewKernelThread(proc, cpu->id, THREAD_QUEUE_CNT - 1, sched_idle);
        }
        atomic_store_8((volatile uint8_t*)&PIT::TickHandle, (uint64_t)(uintptr_t)&PIT::Tick_, 0);
    }

    proc_t *NewProcess(bool user) {
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        if (!proc) return nullptr;
        _memset(proc, 0, sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid,1,ATOMIC_RELAXED);
    
        proc->pagemap = (user ? VMM::NewPM() : kernel_pagemap);
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        if (!proc->FDMan) { kfree(proc); return nullptr; }
        fd_manager_init(proc->FDMan);
        proc->fd_count = 4;
        return proc;
    }

    void PrepareUserStack(thread_t *thread, int32_t argc, char *argv[], char *envp[]) {
        if (argc <= 0) return;
        char **kernel_argv = nullptr;
        char **kernel_envp = nullptr;
        uint64_t *thread_argv = nullptr;
        uint64_t *thread_envp = nullptr;int32_t envc = 0;
        kernel_argv = (char**)kmalloc(argc * sizeof(char*));
        
        uint64_t stack_top = thread->ctx.rsp;
        uint64_t offset = 0;
        pagemap_t *restore;
        if (!kernel_argv) return;
        for (int32_t i = 0; i < argc; i++) kernel_argv[i] = nullptr;

        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(argv[i]) + 1;
            kernel_argv[i] = (char*)kmalloc(size);
            if (!kernel_argv[i]) goto cleanup;
            __memcpy(kernel_argv[i], argv[i], size);
        }

        while (envp[envc++]); envc -= 1;
        kernel_envp = (char**)kmalloc(envc * sizeof(char*));
        if (!kernel_envp) goto cleanup;
        for (int32_t i = 0; i < envc; i++) kernel_envp[i] = nullptr;

        for (int32_t i = 0; i < envc; i++) {
            int32_t size = strlen(envp[i]) + 1;
            kernel_envp[i] = (char*)kmalloc(size);
            if (!kernel_envp[i]) goto cleanup;
            __memcpy(kernel_envp[i], envp[i], size);
        }

        thread_argv = (uint64_t*)kmalloc(argc * sizeof(uint64_t));
        if (!thread_argv) goto cleanup;

        if ((argc + envc) % 2 == 0) offset = 8;
        
        restore = VMM::SwitchPageMap(thread->pagemap);
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(kernel_argv[i]) + 1;
            offset += ALIGN_UP(size, 16); 
            thread_argv[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_argv[i], size);
        }
        thread_envp = kmalloc(envc * 8);
        if(!thread_envp)
            goto cleanup;
        for (int32_t i = 0; i < envc; i++) {
            int32_t size = strlen(kernel_envp[i]) + 1;
            offset += ALIGN_UP(size, 16);
            thread_envp[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_envp[i], size);
        }
        offset += 8; *(uint64_t*)(stack_top - offset) = 0; 
        for (int32_t i = envc - 1; i >= 0; i--) {
            offset += 8; *(uint64_t*)(stack_top - offset) = thread_envp[i];
        }
        offset += 8; *(uint64_t*)(stack_top - offset) = 0; 
        for (int32_t i = argc - 1; i >= 0; i--) {
            offset += 8; *(uint64_t*)(stack_top - offset) = thread_argv[i];
        }
        offset += 8; *(uint64_t*)(stack_top - offset) = argc;
        VMM::SwitchPageMap(restore);
        thread->ctx.rsp = stack_top - offset;

    cleanup:
        if (kernel_argv) {
            for (int32_t i = 0; i < argc; i++) if (kernel_argv[i]) kfree(kernel_argv[i]);
            kfree(kernel_argv);
        }
        if (kernel_envp) {
            for (int32_t i = 0; i < envc; i++) if (kernel_envp[i]) kfree(kernel_envp[i]);
            kfree(kernel_envp);
        }
        if (thread_argv) kfree(thread_argv);
    }

    thread_t *NewKernelThread(proc_t *parent, uint32_t cpu_num, int32_t priority, void *entry) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        if (!thread) return nullptr;
        _memset(thread, 0, sizeof(thread_t));
        
        thread->id = atomic_add_fetch_8(&sched_tid,1,ATOMIC_RELAXED);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->IsForkThread = false;
        thread->pagemap = parent->pagemap;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);
        Schedule::Internal::ProcessAddThread(parent, thread);
        
        cpu_t *cpu = get_cpu(cpu_num);
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP((cpu->XsaveSize), PAGE_SIZE), true);
        if (!thread->fx_area) { kfree(thread); return nullptr; }
        _memset(thread->fx_area, 0, cpu->XsaveSize);
        cpu->OverLoadableFuncs.StoreSIMDState(thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        if (!kernel_stack) { VMM::Free(kernel_pagemap, thread->fx_area); kfree(thread); return nullptr; }
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

        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Internal::InsertToQueue(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);
        return thread;
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
        Schedule::Internal::ProcessAddThread(parent, thread);

        __hmap_s_mp *MP = GetMount(Path);
        if(!MP) { kerrorln("Cannot Find Mount Point!!!"); kfree(thread); return nullptr; }
        
        void *FileDesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
        if (!FileDesc) { kfree(thread); return nullptr; }
        _memset(FileDesc, 0, MP->FSOPS->SIZEOF_FILE_DESC);
        
        if(MP->FSOPS->open(FileDesc, Path, O_RDONLY) != 0) { kfree(FileDesc); kfree(thread); return nullptr; }
        
        uint64_t FSize = MP->FSOPS->fsize(FileDesc);
        uint8_t *buffer = (uint8_t*)kmalloc(FSize);
        if (!buffer) { MP->FSOPS->close(FileDesc); kfree(FileDesc); kfree(thread); return nullptr; }
        MP->FSOPS->read(FileDesc, buffer, FSize, 0);
        MP->FSOPS->close(FileDesc);
        kfree(FileDesc);
        
        uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
        _memset(&thread->ctx, 0, sizeof(context_t));
        thread->ctx.rip = elf_load(buffer, thread->pagemap, &tls_offset, &tls_memsz, &tls_filesz, &tls_align); 
        
        if (thread->ctx.rip == 0) {
            kerrorln("ELF load failed!");
            kfree(buffer); kfree(thread); return nullptr; 
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
            __memcpy((void*)(tcb_base - ALIGN_UP(tls_memsz, tls_align)), (void*)(buffer + tls_offset), tls_filesz);
            *(uint64_t*)tcb_base = tcb_base; 
            VMM::SwitchPageMap(kernel_pagemap); 
            thread->fs = tcb_base;
            thread->tls_base = tls_mem; // 记录以便销毁
            thread->tls_pages = tls_pages;
        }

        kfree(buffer);
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
        
        cpu_t *cpu = get_lw_cpu();
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        __memcpy(thread->fx_area, parent->fx_area, cpu->XsaveSize);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        __memcpy((void*)kernel_stack, (void*)parent->kernel_stack, 4 * PAGE_SIZE);

        thread->id = atomic_add_fetch_8(&sched_tid,1,ATOMIC_RELAXED);
        
        thread->cpu_num = cpu->id;
        thread->parent = proc;
        thread->IsForkThread = true; // 标记为 Fork 线程，防止双释放栈
        thread->pagemap = proc->pagemap; 
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + 4 * PAGE_SIZE;
        thread->stack = parent->stack; 
        thread->sig_stack = parent->sig_stack;
        thread->tls_base = parent->tls_base;
        thread->tls_pages = parent->tls_pages;

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
        __memcpy(proc->sig_handlers, parent->sig_handlers, 64 * sizeof(sigaction_t));
        
        // 深拷贝 FDMan 防止双重释放
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        __memcpy(proc->FDMan, parent->FDMan, sizeof(fd_manager_t));
        proc->fd_count = parent->fd_count;
        return proc;
    }


    thread_t* this_thread() {
        cpu_t* cpu = this_cpu();
        return cpu ? cpu->current_thread : nullptr;
    }

    proc_t *this_proc() {
        thread_t* t = this_thread();
        return t ? t->parent : nullptr;
    }
    
    void Yield() {
        LAPIC::StopTimer();
        asm volatile("int %0" :: "i"(SCHED_VEC + 1));
    }

    void PAUSE() { LAPIC::StopTimer(); }
    
    void Resume() {
        cpu_t* cpu = this_cpu();
        if (cpu) LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1); 
    }
}