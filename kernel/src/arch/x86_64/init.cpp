#include "interrupt/idt.h"
#include "interrupt/gdt.h"
#include "../pinc.h"
#include "../../mem/pmm.h"
#include "../../mem/vmm.h"
#include "../../mem/heap.h"
#include "../../klib/klib.h"
#include "pit/pit.h"

void x86_64_init(void){
    WELCOME_X86_64
    kinfo("INIT x86_64 ARCH\n");

    kinfo("CPU TSC DATA: %ld\n",rdtsc());

    kinfo("INIT GDT...\n");
    gdt_init();
    kpok("GDT INIT!\n");

    kinfo("INIT IDT...\n");
    idt_init();
    kpok("IDT INIT!\n");

    kinfo("INIT PMM...\n");
    PMM::Init();
    kpok("PMM INIT!\n");

    kinfo("INIT VMM...\n");
    VMM::Init();
    kpok("VMM INIT!\n");

    kinfo("INIT PIT...\n");
    PIT::Init();
    kpok("PIT INIT!\n");


    void* stack = HIGHER_HALF(pmm_alloc(3) + (3 * PAGE_SIZE));
    tss_list[0].rsp[0] = (u64)stack;

    Heap::Init();
    kpok("KHeap initialised.\n");

}