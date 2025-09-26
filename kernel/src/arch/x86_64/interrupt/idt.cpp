#include <arch/x86_64/interrupt/idt.h>
#include <print/e9print.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/schedule/sched.h>

extern void FT_Clear();

#pragma GCC push_options
#pragma GCC optimize ("O0")

__attribute__((aligned(0x10))) static idt_entry_t idt_entries[256];
static idt_desc_t idt_desc;
extern "C" void *idt_int_table[];
void *handlers[224];


static const char* isr_errors[32] = {
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
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved"
};

void idt_set_entry(uint16_t vector, void *isr, uint8_t flags);

void idt_init() {
    for (uint16_t vector = 0; vector < 256; vector++)
        idt_set_entry(vector, idt_int_table[vector], 0x8e);

    idt_desc.size = sizeof(idt_entries) - 1;
    idt_desc.address = (uint64_t)&idt_entries;

    __asm__ volatile ("lidt %0" : : "m"(idt_desc) : "memory");
    __asm__ volatile ("sti");
}

void idt_reinit() {
    __asm__ volatile ("lidt %0" : : "m"(idt_desc) : "memory");
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

extern "C"  void idt_install_irq(uint8_t irq, void *handler) {
    kpokln("IRQ %d Installed",irq);
    handlers[irq] = handler;
    if (irq < 16){IOAPIC::RemapIRQ(0, irq, irq + 32, false);}
    // Right now we just map
    // every interrupt to MP.
}

extern "C" void idt_irq_handler(context_t *ctx) {
    void (*handler)(context_t*) = handlers[ctx->int_no - 32];
    if (!handler) {
        kerror("(PANIC)Uncaught IRQ #%d.\n", ctx->int_no - 32);
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
        uint8_t should_halt = VMM::HandlePF(ctx);
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
        kinfo("On thread %d\n", this_cpu()->current_thread->id);
    stackframe_t *stack;
    __asm__ volatile ("movq %%rbp, %0" : "=r"(stack));
    kerror("Stack trace:\n");
    for (int32_t i = 0; i < 5 && stack; i++) {
        kerror("    0x%p\n", stack->rip);
        stack = stack->rbp;
    }
    // TODO: Dump registers.
    asm volatile("cli");
    hcf();
}

#pragma GCC pop_options