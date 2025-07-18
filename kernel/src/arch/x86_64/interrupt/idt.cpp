#include <arch/x86_64/interrupt/idt.h>
#include <print/e9print.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/lapic/lapic.h>

__attribute__((aligned(0x10))) static idt_entry idt_entries[256];
static idtr idt;
extern void* idt_int_table[];

//void* irq_handlers[256] = {};
IRQDesc irqe[256] = {0};

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

void idt_init() {
    for (uint16_t vec = 0; vec < 256; vec++) {
        idt_set_entry(vec, idt_int_table[vec], 0x8E, (vec == 0x80 ? 0 : 0));
    }

    idt = (idtr){
        .size   = (uint16_t)sizeof(idt_entry) * 256 - 1,
        .offset = (uint64_t)&idt_entries
    };

    __asm__ volatile ("lidt %0" : : "m"(idt) : "memory");
    __asm__ volatile ("sti");
}

void idt_reinit() {
    __asm__ ("lidt %0" : : "m"(idt) : "memory");
    __asm__ ("sti");
}

void irq_register(u8 vec, void* handler) {
    if (vec < 15)
        IOAPIC::RedirectIRQ(bsp_lapic_id, vec + 32, vec, false);
    //irq_handlers[vec] = handler;
    irqe[vec].handler = handler;
    irqe[vec].CpuId = LAPIC::GetID();
    irqe[vec].VecId = vec;
}

void irq_unregister(u8 vec) {
    //irq_handlers[vec] = NULL;
    irqe[vec].handler = NULL;
}

void backtrace() {
    struct stackframe *stk;
    __asm__ volatile ("mov %%rbp, %0" : "=r"(stk));
    kprintf("  %lx", stk->rip);
    stk = stk->rbp;
    hcf();
}

void idt_set_entry(u8 vec, void* isr, u8 type, u8 dpl) {
    idt_entries[vec].low  = (u64)isr & 0xFFFF;
    idt_entries[vec].cs   = 0x28;
    idt_entries[vec].ist  = 0;
    idt_entries[vec].type = type;
    idt_entries[vec].dpl  = dpl;
    idt_entries[vec].p    = 1;
    idt_entries[vec].mid  = ((u64)isr >> 16) & 0xFFFF;
    idt_entries[vec].high = ((u64)isr >> 32) & 0xFFFFFFFF;
    idt_entries[vec].resv = 0;
}

extern "C" void irq_handler(registers* r) {
    void(*handler)(registers*);
    //handler = (void(*)(registers*))irq_handlers[r->int_no - 32];
    handler = (void(*)(registers*))(irqe[r->int_no - 32].handler);

    if (handler != NULL)
        handler(r);
}

extern "C" void isr_handler(registers* r) {
    if (r->int_no == 0xff)
        return; // Spurious interrupt
    
    if (r->int_no == 14) {
        // Page fault
        if (!VMM::HandlePF(r))
            return;
    }
    
    __asm__ volatile ("cli");
    kerror("isr_handler(): "
        "System fault! %s. "
        "RIP: %llx. CS: %x SS: %x\n", isr_errors[r->int_no], r->rip, r->cs, r->ss);
    hcf();
}