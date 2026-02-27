/*
* SPDX-License-Identifier: GPL-2.0-only
* File: idt.cpp
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
//static idt_desc_t idt_desc;
extern "C" void *idt_int_table[];
void *handlers[256];


static constexpr char* isr_errors[32] = {
    "Division by zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Detected overflow",
    "Out-of-bounds",
    "Invalid opcode",
    "No coprocessor",
    "Double fault",
    "Coprocessor segment overrun",
    "Bad TSS",
    "Segment not present",
    "Stack fault",
    "General protection fault",
    "Page fault",
    "Unknown interrupt",
    "Coprocessor fault",
    "Alignment check",
    "Machine check",
    "Reserved ISR ERR NO.19",
    "Reserved ISR ERR NO.20",
    "Reserved ISR ERR NO.21",
    "Reserved ISR ERR NO.22",
    "Reserved ISR ERR NO.23",
    "Reserved ISR ERR NO.24",
    "Reserved ISR ERR NO.25",
    "Reserved ISR ERR NO.26",
    "Reserved ISR ERR NO.27",
    "Reserved ISR ERR NO.28",
    "Reserved ISR ERR NO.29",
    "Reserved ISR ERR NO.30",
    "Reserved ISR ERR NO.31"
};

void idt_set_entry(uint16_t vector, void *isr, uint8_t flags);

void idt_init() {
    for (uint16_t vector = 0; vector < 256; vector++)
        idt_set_entry(vector, idt_int_table[vector], 0x8e);

    smp_cpu_list[smp_bsp_cpu]->idtdesc.size = sizeof(idt_entries) - 1;
    smp_cpu_list[smp_bsp_cpu]->idtdesc.address = (uint64_t)&idt_entries;
    smp_cpu_list[smp_bsp_cpu]->handlers = (uint64_t)handlers;

    __asm__ volatile ("lidt %0" : : "m"(smp_cpu_list[smp_bsp_cpu]->idtdesc) : "memory");
    __asm__ volatile ("sti");
}

void idt_reinit(uint32_t CPUID) {
    idt_entry_t* local_idt = (idt_entry_t*)kmalloc(sizeof(idt_entries));
    __memcpy(local_idt, (void*)&idt_entries, sizeof(idt_entries));
    
    smp_cpu_list[CPUID]->idtdesc.size = sizeof(idt_entries) - 1;
    smp_cpu_list[CPUID]->idtdesc.address = (uint64_t)local_idt;
    smp_cpu_list[CPUID]->handlers = kmalloc(256 * sizeof(uint64_t));
    __asm__ volatile ("lidt %0" : : "m"(smp_cpu_list[CPUID]->idtdesc) : "memory");
    __asm__ volatile ("sti");
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

extern "C"  void idt_install_irq(uint8_t irq, void *handler) {
    kpokln("IRQ %d Installed",irq);
    smp_cpu_list[smp_bsp_cpu]->handlers[irq] = (uint64_t)handler;
    //handlers[irq] = handler;
    //if (irq < 16){IOAPIC::RemapIRQ(0, irq, irq + 32, false);}
    // Right now we just map
    // every interrupt to MP.
}

extern "C" void idt_install_irq_cpu(uint32_t cpuid,uint8_t irq, void* handler) {
    smp_cpu_list[cpuid]->handlers[irq] = (uint64_t)handler;
}

extern "C" void idt_irq_handler(context_t *ctx) {
    void (*handler)(context_t*) = this_cpu()->handlers[ctx->int_no];
    if (!handler) {
        kerror("(PANIC)Uncaught IRQ #%d.\n", ctx->int_no);
    }
    handler(ctx);
}

extern struct flanterm_context* ft_ctx;

extern "C" void idt_exception_handler(context_t *ctx) {
    uint64_t cr0,cr2,cr3,cr4;
    if (ctx->int_no >= 32)
        return idt_irq_handler(ctx);
    if (ctx->int_no == 14) {
        LAPIC::StopTimer();
        uint32_t should_halt = VMM::HandlePF(ctx);
        if (!should_halt) {
            Schedule::Yield();
            return;
        }
    }
    if(ctx->int_no == 14){
        __asm__ volatile ("movq %%cr2, %0" : "=r"(cr2));
        kerror("Page fault on 0x%p, Should NOT continue.\n", cr2);
    }
    __asm__ volatile ("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("movq %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("movq %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("movq %%cr4, %0" : "=r"(cr4));
    kerror("Kernel exception caught: %s.\n", isr_errors[ctx->int_no]);
    kerror("Kernel crash on core %d at 0x%p.\n", smp_started ? this_cpu()->id : 0,
        ctx->rip);
    kerrorln("REGISTERS DATA(64bits data):");
    kerrorln("RAX: 0x%p  RBX: 0x%p RCX: 0x%p RDX: 0x%p.",ctx->rax,ctx->rbx,ctx->rcx,ctx->rdx);
    kerrorln("RBP: 0x%p  RDI: 0x%p RSI: 0x%p RSP: 0x%p.",ctx->rbp,ctx->rdi,ctx->rsi,ctx->rsp);
    kerrorln("CR0: 0x%p  CR4: 0x%p CR3: 0x%p CR2: 0x%p",cr0,cr4,cr3,cr2);
    kerrorln("RFLAGS: 0x%p",ctx->rflags);
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