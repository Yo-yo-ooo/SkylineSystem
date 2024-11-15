#include "interrupt/idt.h"
#include "interrupt/gdt.h"
#include "../pinc.h"
#include "../../mem/pmm.h"
#include "../../mem/vmm.h"
#include "../../mem/heap.h"
#include "../../klib/klib.h"
#include "pit/pit.h"
#include "rtc/rtc.h"

void sse_enable() {
    u64 cr0;
    u64 cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0) : : "memory");
    cr0 &= ~((u64)1 << 2);
    cr0 |= (u64)1 << 1;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    __asm__ volatile("mov %%cr4, %0" :"=r"(cr4) : : "memory");
    cr4 |= (u64)3 << 9;
    __asm__ volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
}

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

    kinfo("INIT SSE\n");
    sse_enable();
    kpok("SSE INIT!\n");

    kinfo("INIT PIT...\n");
    PIT::InitPIT();
    kpok("PIT INIT!\n");

    kinfo("INIT RTC...\n");
    RTC::InitRTC();
    kpok("RTC INIT!\n");

    kinfo("INIT HEAP...\n");

    void* stack = HIGHER_HALF(pmm_alloc(3) + (3 * PAGE_SIZE));
    tss_list[0].rsp[0] = (u64)stack;

    Heap::Init();
    kpok("KHeap initialised.\n");

}