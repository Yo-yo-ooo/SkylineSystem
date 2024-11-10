#include "interrupt/idt.h"
#include "interrupt/gdt.h"
#include "../pinc.h"
#include "../../mem/pmm.h"
#include "vmm/vmm.h"
#include "../../klib/klib.h"
#include "pit/pit.h"

void x86_64_init(void){
    WELCOME_X86_64
    kprintf("[INFO] INIT x86_64 ARCH\n");

    kprintf("[INFO] CPU TSC DATA: %ld\n",rdtsc());

    kprintf("[INFO] INIT GDT...\n");
    gdt_init();
    kprintf("[ OK ] GDT INIT!\n");

    kprintf("[INFO] INIT IDT...\n");
    idt_init();
    kprintf("[ OK ] IDT INIT!\n");

    e9_printf("[INFO] INIT PMM...");
    PMM::Init();
    e9_printf("[ OK ] PMM INIT!");

    e9_printf("[INFO] INIT VMM...");
    vmm_init();
    e9_printf("[ OK ] VMM INIT!");

    e9_printf("[INFO] INIT PIT...");
    PIT::Init();
    e9_printf("[ OK ] PIT INIT!");

}