#include "interrupt/idt.h"
#include "interrupt/gdt.h"
#include "../pinc.h"
#include "../../mem/pmm.h"

void x86_64_init(void){
    e9_printf("x86_64_init() called!");

    e9_printf("INIT GDT...");
    gdt_init();
    e9_printf("GDT INIT!");

    e9_printf("INIT IDT...");
    idt_init();
    e9_printf("IDT INIT!");

    

    WELCOME_X86_64
}