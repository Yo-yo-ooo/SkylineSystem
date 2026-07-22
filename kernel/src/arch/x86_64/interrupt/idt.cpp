//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/interrupt/idt.h>
#include <print/e9print.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/schedule/sched.h>

extern void FT_Clear();

#pragma GCC push_options
#pragma GCC optimize ("O0")

volatile static idt_entry_t alignas(16) idt_entries[256];
extern "C" void *idt_int_table[];
interrupt_handler_t handlers[256];

extern volatile const char* isr_errors[32];

void idt_set_entry(uint16_t vector, void *isr, uint8_t flags);

void idt_init() {
    for (uint16_t vector = 0; vector < 256; vector++) {
        idt_set_entry(vector, idt_int_table[vector], 0x8e);
    }

    smp_cpu_list[smp_bsp_cpu]->idtdesc.size = sizeof(idt_entries) - 1;
    smp_cpu_list[smp_bsp_cpu]->idtdesc.address = (uint64_t)&idt_entries;
    memcpy_fscpuf(smp_cpu_list[smp_bsp_cpu]->handlers, (void*)handlers, 256 * sizeof(uint64_t));
    
    // 【修复 3】标记 0~127 号向量为系统保留，彻底防止动态分配冲突
    smp_cpu_list[smp_bsp_cpu]->IntrBitMap[0] = 0xFFFFFFFFFFFFFFFFULL;
    smp_cpu_list[smp_bsp_cpu]->IntrBitMap[1] = 0xFFFFFFFFFFFFFFFFULL;
    smp_cpu_list[smp_bsp_cpu]->IntrBitMap[2] = 0;
    smp_cpu_list[smp_bsp_cpu]->IntrBitMap[3] = 0;

    __asm__ volatile ("lidt %0" : : "m"(smp_cpu_list[smp_bsp_cpu]->idtdesc) : "memory");
    __asm__ volatile ("sti");
}

void idt_reinit(uint32_t CPUID) {
    idt_entry_t* local_idt = (idt_entry_t*)kmalloc(sizeof(idt_entries) + 15);
    uint64_t aligned_addr = ((uint64_t)local_idt + 15) & ~15;
    idt_entry_t* aligned_idt = (idt_entry_t*)aligned_addr;
    
    __memcpy(aligned_idt, (void*)&idt_entries, sizeof(idt_entries));
    
    smp_cpu_list[CPUID]->idtdesc.size = sizeof(idt_entries) - 1;
    smp_cpu_list[CPUID]->idtdesc.address = (uint64_t)aligned_idt;
    memcpy_fscpuf(smp_cpu_list[CPUID]->handlers, (void*)smp_cpu_list[smp_bsp_cpu]->handlers, 256 * sizeof(uint64_t));
    smp_cpu_list[CPUID]->IntrBitMap[0] = 0xFFFFFFFFFFFFFFFFULL;
    smp_cpu_list[CPUID]->IntrBitMap[1] = 0xFFFFFFFFFFFFFFFFULL;
    smp_cpu_list[CPUID]->IntrBitMap[2] = 0;
    smp_cpu_list[CPUID]->IntrBitMap[3] = 0;
    __asm__ volatile ("lidt %0" : : "m"(smp_cpu_list[CPUID]->idtdesc) : "memory");
}

void idt_set_entry(uint16_t vector, void *isr, uint8_t flags) {
    idt_entry_t *entry = &idt_entries[vector];
    entry->offset_low = (uint16_t)((uint64_t)isr & 0xFFFF);
    entry->selector = 0x08;
    entry->ist = 0;
    entry->flags = flags;
    entry->offset_mid = (uint16_t)(((uint64_t)isr >> 16) & 0xFFFF);
    entry->offset_high = (uint32_t)(((uint64_t)isr >> 32) & 0xFFFFFFFF);
    entry->resv = 0;
}

void idt_set_ist(uint16_t vector, uint8_t ist) {
    idt_entries[vector].ist = ist;
}

void idt_set_ist_cpu(uint32_t cpuid,uint16_t vector, uint8_t ist) {
    if(cpuid == smp_bsp_cpu)
        idt_set_ist(vector,ist);
    idt_entry_t* local_idt = (idt_entry_t*)smp_cpu_list[cpuid]->idtdesc.address;
    local_idt[vector].ist = ist;
}

extern "C" uint8_t RequestFreeIRQPerCPU() {
    cpu_t *target_cpu = GetLWIntrCpu();
    if (!target_cpu) {
        kerrorln("RequestFreeIRQPerCPU: No available CPU!");
        return 0xFF;
    }

    for (uint32_t i = 0; i < 4; i++) {
        uint64_t bitmap_val = target_cpu->IntrBitMap[i];
        if (bitmap_val == UINT64_MAX) continue;
        uint32_t j = __builtin_ctzll(~bitmap_val);
        uint64_t mask = 1ULL << j;
        // 【最高优先级修复 1】原子置位，防止并发分配与重复分配
        __sync_or_and_fetch(&target_cpu->IntrBitMap[i], mask);
        return ((i * 64) + j);
    }
    
    kerrorln("RequestFreeIRQPerCPU: No free IRQ vector available!");
    return 0xFF;
}

extern "C"  void idt_install_irq(uint8_t irq, void *handler) {
    if (smp_cpu_list[smp_bsp_cpu]->handlers[irq]) {
        kerrorln("Warning: IRQ %d is already installed, overwriting!", irq);
    }
    kpokln("IRQ %d Installed",irq);
    // 【最高优先级修复 2】遍历所有 CPU 安装处理程序与位图，保证多核一致
    for (int32_t i = 0; i <= smp_last_cpu; i++) {
        if (smp_cpu_list[i] == nullptr) continue;
        smp_cpu_list[i]->handlers[irq] = (uint64_t)handler;
        smp_cpu_list[i]->IntrRegistCount++;
        uint32_t Index = irq / 64;
        uint32_t SIndex = irq % 64;
        SET1_BIT(smp_cpu_list[i]->IntrBitMap[Index], SIndex);
    }
}

extern "C" void idt_install_irq_cpu(uint32_t cpuid,uint8_t irq, void* handler) {
    smp_cpu_list[cpuid]->handlers[irq] = (uint64_t)handler;
    smp_cpu_list[cpuid]->IntrRegistCount++;
    uint32_t Index = irq / 64;
    uint32_t SIndex = irq % 64;
    SET1_BIT(smp_cpu_list[cpuid]->IntrBitMap[Index],SIndex);
}

extern int32_t smp_last_cpu;
extern "C" cpu_t* GetLWIntrCpu(){
    cpu_t *cpu = nullptr;
    for (int32_t i = 0; i <= smp_last_cpu; i++) {
        if (smp_cpu_list[i] == nullptr) continue;
        if (!cpu) {
            cpu = smp_cpu_list[i];
            continue;
        }
        if (smp_cpu_list[i]->IntrRegistCount < cpu->IntrRegistCount)
            cpu = smp_cpu_list[i];
    }
    return cpu;
}

extern "C" void idt_irq_handler(context_t *ctx) {
    cpu_t* cpu = this_cpu();
    if (!cpu || !cpu->handlers) {
        kerror("CPU or Handlers not initialized for IDT %d\n", ctx->int_no);
        if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
        return;
    }
    interrupt_handler_t handler = cpu->handlers[ctx->int_no];
    if (!handler) {
        kerror("(PANIC)Uncaught IRQ #%d.\n", ctx->int_no);
        if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) LAPIC::EOI();
        return;
    }
    handler(ctx);
    // 统一在外部中断出口处发送 EOI
    if (ctx->int_no >= 0x20 && ctx->int_no < 0x40) {
        if (ctx->int_no != SCHED_VEC && ctx->int_no != SCHED_VEC + 1) {
            LAPIC::EOI();
        }
    }
}

extern struct flanterm_context* ft_ctx;
extern "C" void idt_exception_handler(context_t *ctx) {
    cpu_t *cpu = this_cpu();
    bool from_user = ((ctx->cs & 3) == 3);

    if (ctx->int_no == SCHED_VEC || ctx->int_no == SCHED_VEC + 1) {
        if (cpu) cpu->preempt_count++;
        Schedule::Internal::Switch(ctx);
        if (cpu) cpu->preempt_count--;
        return;
    }

    if (cpu) cpu->preempt_count++;

    if (ctx->int_no >= 32) {
        idt_irq_handler(ctx);
        if (cpu) cpu->preempt_count--;
        return;
    }

    if (ctx->int_no == 14) {
        uint32_t should_halt = VMM::HandlePF(ctx);
        if (!should_halt) {
            // 【最高优先级修复 4】无论是否是 idle 线程，缺页处理后都应恢复调度定时器
            if (cpu) {
                uint64_t quantum = cpu->base_quantum;
                if (cpu->current_thread && cpu->current_thread != cpu->idle_thread) {
                    quantum = (cpu->current_thread->custom_quantum > 0) ? cpu->current_thread->custom_quantum : cpu->base_quantum;
                }
                LAPIC::Oneshot(SCHED_VEC, quantum);
            }
            if (cpu) cpu->preempt_count--;
            return;
        }
    }

    if (from_user && ctx->rip < 0x1000) {
        kerrorln("Program exited abnormally (Missing exit syscall?). PID: %d, RIP: 0x%p",
                 Schedule::this_proc() ? Schedule::this_proc()->id : -1, ctx->rip);
        Schedule::Exit(0);
    }

    if (from_user) {
        uint64_t cr2 = 0;
        __asm__ volatile ("movq %%cr2, %0" : "=r"(cr2));
        kerrorln("User process crashed! PID: %d, Exception: %d at RIP: 0x%p (CR2: 0x%p)",
                 Schedule::this_proc() ? Schedule::this_proc()->id : -1, 
                 ctx->int_no, ctx->rip, cr2);
        Schedule::Exit(-1);
    }

    uint64_t cr0,cr2,cr3,cr4;
    __asm__ volatile ("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("movq %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("movq %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("movq %%cr4, %0" : "=r"(cr4));
    
    kerror("Kernel exception caught: %s.\n", isr_errors[ctx->int_no]);
    kerror("Kernel crash on core %d at 0x%p.\n", smp_started ? this_cpu()->lapic_id : 0, ctx->rip);
    kerrorln("REGISTERS DATA(64bits data):");
    kerrorln("RAX: 0x%p  RBX: 0x%p RCX: 0x%p RDX: 0x%p.",ctx->rax,ctx->rbx,ctx->rcx,ctx->rdx);
    kerrorln("RBP: 0x%p  RDI: 0x%p RSI: 0x%p RSP: 0x%p.",ctx->rbp,ctx->rdi,ctx->rsi,ctx->rsp);
    kerrorln("CR0: 0x%p  CR4: 0x%p CR3: 0x%p CR2: 0x%p",cr0,cr4,cr3,cr2);
    kerrorln("RFLAGS: 0x%p",ctx->rflags);
    kerrorln("ERROR_CODE: 0x%p",ctx->error_code);
    kerrorln("CS: 0x%x SS: 0x%x", ctx->cs, ctx->ss);
    
    if (smp_started && this_cpu()->current_thread)
        kerrorln("On thread %d", this_cpu()->current_thread->id);
    stackframe_t *stack;
    __asm__ volatile ("movq %%rbp, %0" : "=r"(stack));
    kerror("Stack trace:\n");
    for (int32_t i = 0; i < 5 && stack; i++) {
        kerror("    0x%p\n", stack->rip);
        stack = stack->rbp;
    }
    asm volatile("cli");
    hcf();
}

#pragma GCC pop_options