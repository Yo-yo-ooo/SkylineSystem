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

//static spinlock_t pid_tree_lock = 0;
static uint64_t sched_tid = 0;
static volatile uint64_t sched_pid = 0; 

extern uint64_t elf_load(uint8_t *data, pagemap_t *pagemap, 
                  uint64_t *tls_offset = nullptr, 
                  uint64_t *tls_memsz = nullptr, 
                  uint64_t *tls_filesz = nullptr, 
                  uint64_t *tls_align = nullptr);

void sched_idle() {
    while (true) {
        Schedule::Yield();
    }
}

static cpu_t *get_lw_cpu() {
    cpu_t *lw_cpu = nullptr;
    for (int32_t i = 0; i <= smp_last_cpu; i++) {
        cpu_t *cpu = smp_cpu_list[i];
        if (cpu == nullptr) continue; 
        if (!lw_cpu) { lw_cpu = cpu; continue; }
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
            if (thread->priority == THREAD_QUEUE_CNT - 1)
                return 1;
            thread_queue_t *old_queue = &cpu->thread_queues[thread->priority];
            thread->priority++;
            thread_queue_t *new_queue = &cpu->thread_queues[thread->priority];
            if (thread->list_next == thread) {
                old_queue->head = nullptr;
                old_queue->current = nullptr;
            } else {
                if (old_queue->head == thread)
                    old_queue->head = thread->list_next;
                if (old_queue->current == thread)
                    old_queue->current = thread->list_next;
                thread->list_next->list_prev = thread->list_prev;
                thread->list_prev->list_next = thread->list_next;
            }
            --cpu->thread_count;
            Schedule::Internal::AddThread(cpu, thread);
            return 0;
        }

        void BoostPriority(cpu_t *cpu) {
            // 1. 统计瞬时活跃线程数、总线程数以及低优先级队列积压数
            uint32_t active_count = 0;
            uint32_t pending_in_lower = 0;
            
            for (uint32_t i = 0; i < THREAD_QUEUE_CNT; i++) {
                thread_queue_t *q = &cpu->thread_queues[i];
                if (!q->head) continue;
                
                thread_t *curr = q->head;
                do {
                    if (curr->state == THREAD_RUNNING) {
                        active_count++;
                        // 如果在非最高优先级队列里发现活跃线程，记为积压
                        if (i > 0) pending_in_lower++;
                    }
                    curr = curr->list_next;
                } while (curr != q->head);
            }

            // 2. 更新历史负载 EMA
            cpu->load_ema = (cpu->load_ema * 7 + active_count) / 8;
            if (cpu->load_ema == 0) return; // 系统空闲，不计算

            // 3. 计算动态阈值：基数 / EMA
            // 基数不再是写死的 2000，而是由 cpu->boost_interval_base 动态调整
            uint64_t dynamic_threshold = cpu->boost_interval_base / cpu->load_ema;
            if (dynamic_threshold < 2) dynamic_threshold = 2; // 物理下限保护

            // 4. 检查是否达到提升周期
            if (cpu->tick_count - cpu->last_boost_tick < dynamic_threshold) {
                return; 
            }

            cpu->last_boost_tick = cpu->tick_count;

            if (pending_in_lower == 0) {
                // (Additive Increase) 积压为0，说明提升太早了，白跑一趟。
                // 增大基数，拉长下一次提升间隔。
                cpu->boost_interval_base += 50; 
            } else if (pending_in_lower > cpu->load_ema) {
                // (Multiplicative Decrease) 积压线程甚至超过了平均活跃数，说明严重饿死！
                // 立即减半基数，极大缩短下一次提升间隔。
                cpu->boost_interval_base /= 2;
            }

            // 限制基数在合理范围内，防止极端值 [20, 20000]
            if (cpu->boost_interval_base < 20) cpu->boost_interval_base = 20;
            if (cpu->boost_interval_base > 20000) cpu->boost_interval_base = 20000;

            // 6. 批量链表合并提升 (O(1) 时间复杂度合并)
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
            uint64_t rflags;
            asm volatile("pushfq\n\t""pop %0\n\t""cli" : "=r"(rflags) :: "memory");
            LAPIC::StopTimer();
            
            cpu_t *cpu = this_cpu();
            if (!cpu || !cpu->thread_count || cpu->preempt_count > 0) {
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                asm volatile("push %0\n\t""popfq" :: "r"(rflags) : "memory");
                return;
            }

            // 记录时间节拍
            cpu->tick_count++;

            spinlock_lock(&cpu->sched_lock);
            
            // 执行老化检查（在锁内安全地操作队列）
            Schedule::Internal::BoostPriority(cpu);
            
            thread_t *curr_thread = cpu->current_thread;
            if (curr_thread) {
                curr_thread->fs = rdmsr(FS_BASE);
                curr_thread->ctx = *ctx;
                cpu->OverLoadableFuncs.StoreSIMDState(curr_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
            }
            
            thread_t *next_thread = Schedule::Internal::Pick(cpu);
            if (!next_thread) {
                spinlock_unlock(&cpu->sched_lock);
                if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
                asm volatile("push %0\n\t""popfq" :: "r"(rflags) : "memory");
                return; 
            }
            
            cpu->current_thread = next_thread;
            *ctx = next_thread->ctx;
            TSS::SetRSP(cpu->id, 0, (void*)next_thread->kernel_rsp);
            cpu->kernel_stack = next_thread->kernel_rsp;
            
            // 跳过同进程的页表切换
            if (!curr_thread || curr_thread->pagemap != next_thread->pagemap) {
                VMM::SwitchPageMap(next_thread->pagemap);
            }
            
            cpu->OverLoadableFuncs.WRFSBASE(next_thread->fs);
            cpu->OverLoadableFuncs.LoadSIMDState(next_thread->fx_area, cpu->XsaveMaskLo, cpu->XsaveMaskHi);
            
            spinlock_unlock(&cpu->sched_lock);
            
            uint64_t final_ticks = cpu->thread_queues[next_thread->priority].quantum;
            LAPIC::Oneshot(SCHED_VEC, final_ticks);
            
            if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
            asm volatile("push %0\n\t""popfq" :: "r"(rflags) : "memory");
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
        idt_install_irq(48, (void*)Schedule::Internal::Preempt);
        idt_install_irq(49, (void*)Schedule::Internal::Switch);
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

    uint64_t sched_pid;
    proc_t *NewProcess(bool user) {
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid, 1, 0); 
        proc->threads = nullptr;
        proc->parent = nullptr;
        proc->children = proc->sibling = nullptr;
        proc->pagemap = (user ? VMM::NewPM() : kernel_pagemap);
        _memset(proc->sig_handlers, 0, 64 * sizeof(sigaction_t));
        proc->FDMan = (fd_manager_t*)kmalloc(sizeof(fd_manager_t));
        fd_manager_init(proc->FDMan);
        proc->fd_count = 4;
        return proc;
    }

    void PrepareUserStack(thread_t *thread, int32_t argc, char *argv[], char *envp[]) {
        char **kernel_argv = (char**)kmalloc(argc * 8);
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(argv[i]) + 1;
            kernel_argv[i] = (char*)kmalloc(size);
            __memcpy(kernel_argv[i], argv[i], size);
        }
        int32_t envc = 0;
        while (envp[envc++]); envc -= 1;
        char **kernel_envp = (char**)kmalloc(envc * 8);
        for (int32_t i = 0; i < envc; i++) {
            int32_t size = strlen(envp[i]) + 1;
            kernel_envp[i] = (char*)kmalloc(size);
            __memcpy(kernel_envp[i], envp[i], size);
        }
        uint64_t *thread_argv = (uint64_t*)kmalloc(argc * sizeof(uint64_t));
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
        thread->id = atomic_add_fetch_8(&sched_tid, 1, 0);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->IsForkThread = false;
        thread->pagemap = parent->pagemap;
        thread->flags = 0;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);
        Schedule::Internal::ProcessAddThread(parent, thread);
        
        thread->sig_deliver = 0;
        thread->sig_mask = 0;
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
        thread->fs = 0;

        thread->state = THREAD_RUNNING;

        cpu_t *target_cpu = get_cpu(cpu_num);
        spinlock_lock(&target_cpu->sched_lock);
        target_cpu->has_runnable_thread = true; // 在锁内修改共享状态
        Schedule::Internal::AddThread(target_cpu, thread);
        spinlock_unlock(&target_cpu->sched_lock);

        return thread;
    }

    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int32_t priority, const char *Path, int32_t argc, char *argv[], char *envp[]) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        thread->id = atomic_add_fetch_8(&sched_tid, 1, 0);
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->IsForkThread = false;
        thread->pagemap = parent->pagemap;
        thread->flags = 0;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);
        Schedule::Internal::ProcessAddThread(parent, thread);
        thread->sig_deliver = 0;
        thread->sig_mask = 0;

        __hmap_s_mp *MP = GetMount(Path);
        if(!MP) { kerrorln("Cannot Find Mount Point!!!"); return nullptr; }
        uint64_t FileDescSize = MP->FSOPS->SIZEOF_FILE_DESC;
        void *FileDesc = kmalloc(FileDescSize);
        if (!FileDesc) { kerrorln("kmalloc FileDesc failed"); return nullptr; }
        _memset(FileDesc, 0, FileDescSize);
        int32_t open_rc = MP->FSOPS->open(FileDesc, Path, O_RDONLY);
        if(open_rc != 0) { kerrorln("open file failed, ret = %d", open_rc); kfree(FileDesc); return nullptr; }
        uint64_t FSize = MP->FSOPS->fsize(FileDesc);
        uint8_t *buffer = (uint8_t*)kmalloc(FSize);
        if (!buffer) { kfree(FileDesc); kerrorln("kmalloc buffer failed"); return nullptr; }
        _memset(&thread->ctx, 0, sizeof(context_t));
        MP->FSOPS->read(FileDesc, buffer, FSize, 0);
        
        uint64_t tls_offset = 0, tls_memsz = 0, tls_filesz = 0, tls_align = 0;
        thread->ctx.rip = elf_load(buffer, thread->pagemap, &tls_offset, &tls_memsz, &tls_filesz, &tls_align); 
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
        
        kpokln("Add Thread!");
        return thread;
    }

    thread_t *ForkThread(proc_t *proc, thread_t *parent, void *frame) {
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        
        cpu_t *cpu = get_lw_cpu();
        thread->fx_area = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), true);
        __memcpy(thread->fx_area, parent->fx_area, cpu->XsaveSize);

        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        __memcpy((void*)kernel_stack, (void*)parent->kernel_stack, 4 * PAGE_SIZE);

        // 初始化线程基础信息 (无锁状态)
        thread->id = atomic_add_fetch_8(&sched_tid, 1, 0);
        thread->cpu_num = cpu->id;
        thread->parent = proc;
        thread->IsForkThread = true;
        thread->pagemap = proc->pagemap;
        thread->flags = 0;
        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + 4 * PAGE_SIZE;
        thread->stack = parent->stack;
        thread->sig_stack = parent->sig_stack;

        Schedule::Internal::ProcessAddThread(proc, thread);
        thread->sig_deliver = 0;
        thread->sig_mask = parent->sig_mask;

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
        
        // 极小临界区：仅包含链表插入操作
        spinlock_lock(&cpu->sched_lock);
        cpu->has_runnable_thread = true;
        Schedule::Internal::AddThread(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);

        return thread;
    }

    proc_t *ForkProcess() {
        proc_t *parent = Schedule::this_proc();
        if (!parent) return nullptr; // 防御性检查
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        proc->id = atomic_add_fetch_8(&sched_pid, 1, 0);
        proc->threads = nullptr;
        proc->parent = parent;
        proc->sibling = nullptr;
        if (!parent->children) parent->children = proc;
        else {
            proc_t *last_sibling = parent->children;
            while (last_sibling->sibling != nullptr) last_sibling = last_sibling->sibling;
            last_sibling->sibling = proc;
            proc->sibling = nullptr;
        }
        proc->pagemap = VMM::Fork(parent->pagemap);
        __memcpy(proc->sig_handlers, parent->sig_handlers, 64 * sizeof(sigaction_t));
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
        asm volatile("int $49");
    }

    void PAUSE() { LAPIC::StopTimer(); }
    
    void Resume() {
        if (Schedule::this_thread()) {
            Schedule::this_thread()->flags &= ~TFLAGS_PREEMPTED;
            Schedule::this_thread()->preempt_count = 0;
        }
        LAPIC::IPI(this_cpu()->lapic_id, SCHED_VEC + 1); 
    }
}