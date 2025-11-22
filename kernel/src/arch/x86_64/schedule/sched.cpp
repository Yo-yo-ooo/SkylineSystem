#include <arch/x86_64/allin.h>
#include <elf/elf.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/simd/simd.h>
#include <klib/algorithm/queue.h>


static volatile uint64_t sched_tid = 0;
extern uint64_t elf_load(uint8_t *data, pagemap_t *pagemap);
void sched_idle(){
    while (true)
    {Schedule::Yield();}
}

static cpu_t *get_lw_cpu() {
    cpu_t *cpu = nullptr;
    for (int32_t i = 0; i < smp_last_cpu; i++) {
        if (smp_cpu_list[i] == NULL || i == smp_bsp_cpu) continue;
        if (!cpu) {
            cpu = smp_cpu_list[i];
            continue;
        }
        if (smp_cpu_list[i]->thread_count < cpu->thread_count)
            cpu = smp_cpu_list[i];
    }
    return cpu;
}

namespace Schedule{

    uint64_t sched_pid = 0;
    proc_t *sched_proclist[256] = { 0 };

    namespace Useless{

        

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
            Serial::Writelnf("\n\033[38;2;0;255;255m<@%s>:\033[0mDemoted thread %d to queue %d.\n"
                ,__FUNCTION__,thread->id, thread->priority);
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
            Schedule::Useless::AddThread(cpu, thread);
            return 0;
        }

        thread_t *Pick(cpu_t *cpu) {
            for (uint32_t i = 0; i < THREAD_QUEUE_CNT; i++) {
                thread_queue_t *queue = &cpu->thread_queues[i];
                if (!queue->head)
                    continue;
                thread_t *start = queue->head;
                thread_t *thread = queue->current;
                thread_t *next;
                bool found = false;
                do {
                    next = thread->list_next;
                    if (!(thread->state == THREAD_RUNNING)) {
                        thread = next;
                        continue;
                    }
                    if (!(thread->flags & TFLAGS_PREEMPTED)) {
                        thread->preempt_count = 0;
                        found = true;
                        break;
                    }
                    thread->preempt_count++;
                    if (thread->preempt_count == SCHED_PREEMPTION_MAX) {
                        int32_t ret = Schedule::Useless::Demote(cpu, thread);
                        thread->preempt_count = 0;
                        thread->flags &= ~TFLAGS_PREEMPTED;
                        if (ret == 1) {
                            i = 0;
                            break;
                        }
                    } else {
                        found = true;
                        break;
                    }
                    thread = next;
                } while (thread != queue->head);
                if (found) {
                    queue->current = thread->next;
                    thread->flags &= ~TFLAGS_PREEMPTED;
                    return thread;
                }
            }
            return nullptr;
        }

        void Switch(context_t *ctx) {
            //asm volatile("cli");
            
            //bool IsXSAVEAvail = cpuid_is_xsave_avail();
            LAPIC::StopTimer();
            cpu_t *cpu = this_cpu();
            spinlock_lock(&cpu->sched_lock);
            if (cpu->current_thread) {
                thread_t *thread = cpu->current_thread;
                thread->fs = rdmsr(FS_BASE);
                thread->ctx = *ctx;
                if (cpu->SupportXSAVE)
                    asm volatile("xsave %0" : : "m"(*thread->fx_area), "a"(UINT64_MAX), "d"(UINT64_MAX) : "memory");
                else
                    asm volatile("fxsave (%0)" : : "r"(thread->fx_area) : "memory");
                //__asm__ volatile ("fxsave (%0)" : : "r"(thread->fx_area) : "memory");
            }
            thread_t *next_thread = Schedule::Useless::Pick(cpu);
            cpu->current_thread = next_thread;
            *ctx = next_thread->ctx;
            
            VMM::SwitchPageMap(next_thread->pagemap);
            wrmsr(FS_BASE, next_thread->fs);
            wrmsr(KERNEL_GS_BASE, (uint64_t)next_thread);
            if (cpu->SupportXSAVE)
                asm volatile("xrstor %0" : : "m"(*next_thread->fx_area), "a"(UINT64_MAX), "d"(UINT64_MAX) : "memory");
            else
                asm volatile("fxrstor (%0)" : : "r"(next_thread->fx_area) : "memory");
            
            spinlock_unlock(&cpu->sched_lock);
            // An ideal thread wouldn't need the timer to preempt.
            LAPIC::Oneshot(SCHED_VEC, cpu->thread_queues[next_thread->priority].quantum);
            //asm volatile("sti");
            LAPIC::EOI();
        }


        void Preempt(context_t *ctx) {
            Schedule::this_thread()->flags |= TFLAGS_PREEMPTED;
            Schedule::this_thread()->preempt_count++;
            Schedule::Useless::Switch(ctx);
        }
    }

    void Init(){
        idt_install_irq(16, (void*)Schedule::Useless::Preempt);
        idt_install_irq(17, (void*)Schedule::Useless::Switch);
        idt_set_ist(SCHED_VEC, 1);
        idt_set_ist(SCHED_VEC + 1, 1);
    }
    
    void Install(){
        for (uint32_t i = 0; i <= smp_last_cpu; i++) {
            cpu_t *cpu = smp_cpu_list[i];
            if (!cpu || cpu->id == smp_bsp_cpu)
                continue;
            proc_t *proc = Schedule::NewProcess(false);
            thread_t *thread = Schedule::NewKernelThread(proc, cpu->id, THREAD_QUEUE_CNT - 1, sched_idle);
        }
    }

    proc_t *NewProcess(bool user){
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        proc->id = sched_pid++;
        //proc->cwd = VFS::root_node;
        proc->threads = nullptr;
        proc->parent = nullptr;
        proc->children = proc->sibling = nullptr;
        proc->pagemap = (user ? VMM::NewPM() : kernel_pagemap);
        _memset(proc->sig_handlers, 0, 64 * sizeof(sigaction_t));
        _memset(proc->fd_table, 0, 256 * 8);
        /* proc->fd_table[0] = fd_open("/dev/pts0", O_RDONLY); */
        /* proc->fd_table[1] = fd_open("/dev/pts0", O_WRONLY); */
        /* proc->fd_table[2] = fd_open("/dev/pts0", O_WRONLY); */
        /* proc->fd_table[3] = fd_open("/dev/serial", O_WRONLY); */
        proc->fd_count = 4;
        sched_proclist[proc->id] = proc;
        return proc;
    }

    void PrepareUserStack(thread_t *thread, int32_t argc, char *argv[], char *envp[]){
        // Copy the arguments and envp into kernel memory
        char **kernel_argv = (char**)kmalloc(argc * 8);
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(argv[i]) + 1;
            kernel_argv[i] = (char*)kmalloc(size);
            __memcpy(kernel_argv[i], argv[i], size);
        }

        int32_t envc = 0;
        while (envp[envc++]);
        envc -= 1;

        char **kernel_envp = (char**)kmalloc(envc * 8);
        for (int32_t i = 0; i < envc; i++) {
            int32_t size = strlen(envp[i]) + 1;
            kernel_envp[i] = (char*)kmalloc(size);
            __memcpy(kernel_envp[i], envp[i], size);
        }

        // Copy the arguments to the thread stack.
        uint64_t thread_argv[argc];
        uint64_t stack_top = thread->ctx.rsp;
        uint64_t offset = 0;
        if ((argc + envc) % 2 == 0) offset = 8;
        pagemap_t *restore = VMM::SwitchPageMap(thread->pagemap);
        for (int32_t i = 0; i < argc; i++) {
            int32_t size = strlen(kernel_argv[i]) + 1;
            offset += ALIGN_UP(size, 16); // Keep aligned to 16 bytes (ABI requirement)
            thread_argv[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_argv[i], size);
        }

        // Copy the environment variables to the thread stack.
        uint64_t thread_envp[envc];
        for (int32_t i = 0; i < envc; i++) {
            int32_t size = strlen(kernel_envp[i]) + 1;
            offset += ALIGN_UP(size, 16);
            thread_envp[i] = stack_top - offset;
            __memcpy((void*)(stack_top - offset), kernel_envp[i], size);
        }

        // Set up argv and argc
        offset += 8;
        *(uint64_t*)(stack_top - offset) = 0; // envp[envc] = nullptr

        for (int32_t i = envc - 1; i >= 0; i--) {
            offset += 8;
            *(uint64_t*)(stack_top - offset) = thread_envp[i];
        }

        offset += 8;
        *(uint64_t*)(stack_top - offset) = 0; // argv[argc] = nullptr

        for (int32_t i = argc - 1; i >= 0; i--) {
            offset += 8;
            *(uint64_t*)(stack_top - offset) = thread_argv[i];
        }

        offset += 8;
        *(uint64_t*)(stack_top - offset) = argc;

        VMM::SwitchPageMap(restore);

        thread->ctx.rsp = stack_top - offset;

        for (int32_t i = 0; i < argc; i++)
            kfree(kernel_argv[i]);
        kfree(kernel_argv);
        for (int32_t i = 0; i < envc; i++)
            kfree(kernel_envp[i]);
        kfree(kernel_envp);
    }


    thread_t *NewKernelThread(proc_t *parent, uint32_t cpu_num, int32_t priority, void *entry){
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        thread->id = sched_tid++;
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->pagemap = parent->pagemap;
        thread->flags = 0;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);
        Schedule::Useless::ProcessAddThread(parent, thread);
        thread->sig_deliver = 0;
        thread->sig_mask = 0;
        thread->heap_size = 0;

        // Fx area
        thread->fx_area = VMM::Alloc(kernel_pagemap, 1, true);
        _memset(thread->fx_area, 0, 512);
        *(uint16_t *)(thread->fx_area + 0x00) = 0x037F;
        *(uint32_t *)(thread->fx_area + 0x18) = 0x1F80;

        // Stack (4 KB)
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
        get_cpu(cpu_num)->has_runnable_thread = true;

        Schedule::Useless::AddThread(get_cpu(cpu_num), thread);

        return thread;
    }

    thread_t *NewThread(proc_t *parent, uint32_t cpu_num, int32_t priority, const char *Path, int32_t argc, char *argv[], char *envp[]){
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        thread->id = sched_tid++;
        thread->cpu_num = cpu_num;
        thread->parent = parent;
        thread->pagemap = parent->pagemap;
        thread->flags = 0;
        thread->priority = (priority > (THREAD_QUEUE_CNT - 1) ? (THREAD_QUEUE_CNT - 1) : priority);

        /* thread->heap = VMM::Alloc(thread->pagemap, 8, true);
        thread->heap_size = 8 * PAGE_SIZE; */

        Schedule::Useless::ProcessAddThread(parent, thread);

        thread->sig_deliver = 0;
        thread->sig_mask = 0;
        thread->heap_size = 0;

        // Load ELF
        static ext4_file f;
        ext4_fopen(&f,Path,"r");
        kinfoln("%d",f.fsize);
        uint8_t *buffer = (uint8_t*)kmalloc(ext4_fsize(&f));
        ext4_fread(&f,buffer,ext4_fsize(&f),NULL);
        thread->ctx.rip = elf_load(buffer, thread->pagemap); 
        ext4_fclose(&f);

        // Fx area
        thread->fx_area = VMM::Alloc(kernel_pagemap, 1, true);
        _memset(thread->fx_area, 0, 512);
        *(uint16_t *)(thread->fx_area + 0x00) = 0x037F;
        *(uint32_t *)(thread->fx_area + 0x18) = 0x1F80;

        // Kernel stack (16 KB)
        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        _memset((void*)kernel_stack, 0, 4 * PAGE_SIZE);

        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + (PAGE_SIZE * 4);

        // Thread stack (32 KB)
        uint64_t thread_stack = (uint64_t)VMM::Alloc(thread->pagemap, 8, true);
        uint64_t thread_stack_top = thread_stack + 8 * PAGE_SIZE;
        thread->stack = thread_stack;

        // Sig stack (4 KB)
        uint64_t sig_stack = (uint64_t)VMM::Alloc(thread->pagemap, 1, true);
        thread->sig_stack = sig_stack;

        // Set up the rest of the registers
        thread->ctx.cs = 0x23;
        thread->ctx.ss = 0x1b;
        thread->ctx.rflags = 0x202;

        // Set up stack (argc, argv, env)
        thread->ctx.rsp = thread_stack_top;
        Schedule::PrepareUserStack(thread, argc, argv, envp);
        thread->thread_stack = thread->ctx.rsp;

        thread->fs = 0;

        thread->state = THREAD_RUNNING;
        get_cpu(cpu_num)->has_runnable_thread = true;

        thread->maped_file_list.Info = kmalloc(sizeof(MapedFileInfo) * 256);
        thread->maped_file_list.MaxCount = 256;
        thread->maped_file_list.UsedCount = 0;
        thread->maped_file_list.NextInfoCount = 0;

        Schedule::Useless::AddThread(get_cpu(cpu_num), thread);
        kpokln("Add Thread!");

        return thread;
    }

    thread_t *ForkThread(proc_t *proc, thread_t *parent, void *frame){
        thread_t *thread = (thread_t*)kmalloc(sizeof(thread_t));
        cpu_t *cpu = get_lw_cpu();
        spinlock_lock(&cpu->sched_lock);
        thread->id = sched_tid++;
        thread->cpu_num = cpu->id;
        thread->parent = proc;
        thread->pagemap = proc->pagemap;
        thread->flags = 0;
        thread->fx_area = VMM::Alloc(kernel_pagemap, 1, true);
        __memcpy(thread->fx_area, parent->fx_area, 512);

        Schedule::Useless::ProcessAddThread(proc, thread);

        thread->sig_deliver = 0;
        thread->sig_mask = parent->sig_mask;

        // Set up RIP
        uint64_t kernel_stack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
        __memcpy((void*)kernel_stack, (void*)parent->kernel_stack, 4 * PAGE_SIZE);

        thread->kernel_stack = kernel_stack;
        thread->kernel_rsp = kernel_stack + 4 * PAGE_SIZE;

        thread->stack = parent->stack;

        // Sig stack (4 KB)
        thread->sig_stack = parent->sig_stack;

        typedef struct syscall_frame_t syscall_frame_t;
        syscall_frame_t *f = (syscall_frame_t*)frame;

        // Set up the rest of the registers
        __memcpy(&thread->ctx, f, sizeof(context_t));
        thread->ctx.rsp = parent->thread_stack;
        thread->ctx.cs = 0x23;
        thread->ctx.ss = 0x1b;
        thread->ctx.rflags = f->r11;
        thread->ctx.rax = 0;
        thread->ctx.rip = f->rcx;

        // Set up stack (argc, argv, env)
        thread->thread_stack = parent->thread_stack;

        thread->fs = rdmsr(FS_BASE);

        thread->state = THREAD_RUNNING;
        cpu->has_runnable_thread = true;
        
        Schedule::Useless::AddThread(cpu, thread);
        spinlock_unlock(&cpu->sched_lock);

        return thread;
    }

    proc_t *ForkProcess(){
        proc_t *parent = Schedule::this_proc();
        proc_t *proc = (proc_t*)kmalloc(sizeof(proc_t));
        proc->id = sched_pid++;
        //proc->cwd = parent->cwd;
        proc->threads = nullptr;
        proc->parent = parent;
        proc->sibling = nullptr;
        if (!parent->children) parent->children = proc;
        else {
            proc_t *last_sibling = parent->children;
            while (last_sibling->sibling != nullptr)
                last_sibling = last_sibling->sibling;
            last_sibling->sibling = proc;
            proc->sibling = nullptr;
        }
        proc->pagemap = VMM::Fork(parent->pagemap);
        __memcpy(proc->sig_handlers, parent->sig_handlers, 64 * sizeof(sigaction_t));
        __memcpy(proc->fd_table, parent->fd_table, 256 * 8);
        proc->fd_count = parent->fd_count;
        sched_proclist[proc->id] = proc;
        return proc;
    }

    thread_t *this_thread(){
        if (!this_cpu()) return nullptr;
        return this_cpu()->current_thread;
    }

    proc_t *this_proc(){
        return this_cpu()->current_thread->parent;
    }
    void Exit(int32_t code){
        LAPIC::StopTimer();
        thread_t *thread = Schedule::this_thread();
        thread->state = THREAD_ZOMBIE;
        thread->exit_code = code;
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
        Schedule::Yield();
    }

    void Sleep(uint64_t ms){
        LAPIC::StopTimer();
        Schedule::this_thread()->state = THREAD_SLEEPING;
        Schedule::this_thread()->sleeping_time = ms;
        Schedule::Yield();
    }

    void Yield(){
        LAPIC::StopTimer();
        Schedule::this_thread()->flags &= ~TFLAGS_PREEMPTED;
        Schedule::this_thread()->preempt_count = 0;
        LAPIC::IPI(this_cpu()->id, SCHED_VEC + 1);
    }

    void PAUSE(){
        LAPIC::StopTimer();
    }
    void Resume(){
        if (Schedule::this_thread()) {
            Schedule::this_thread()->flags &= ~TFLAGS_PREEMPTED;
            Schedule::this_thread()->preempt_count = 0;
        }
        LAPIC::IPI(this_cpu()->id, SCHED_VEC + 1);
    }
}