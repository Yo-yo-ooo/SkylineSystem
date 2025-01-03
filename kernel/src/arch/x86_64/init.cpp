#include "interrupt/idt.h"
#include "interrupt/gdt.h"
#include "../pinc.h"
#include "../../mem/pmm.h"
#include "vmm/vmm.h"
#include "../../mem/heap.h"
#include "../../klib/klib.h"
#include "pit/pit.h"
#include "rtc/rtc.h"
#include "../../acpi/madt.h"
#include "../../acpi/acpi.h"
#include "lapic/lapic.h"
#include "ioapic/ioapic.h"
#include "cpuid.h"
#include "schedule/sched.h"
#include "schedule/syscall.h"
#include "dev/ata/ata.h"
#include "MStack/MStackM.h"
#include "MStack/MStackS.h"
#include "dev/pci/pci.h"
#include "../../klib/renderer/rnd.h"
#include "dev/ahci/ahci.h"

void __init x86_64_init(void){
    WELCOME_X86_64
    kinfo("INIT x86_64 ARCH\n");

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

    MStackData::stackPointer = 0;
    for (int i = 0; i < 1000; i++)
        MStackData::stackArr[i] = MStack();
    MStackData::BenchmarkEnabled = false;

    kinfo("INIT SSE\n");
    sse_enable();
    kpok("SSE INIT!\n");

    kinfo("INIT PIT...\n");
    PIT::InitPIT();
    kpok("PIT INIT!\n");

    kinfo("INIT RTC...\n");
    RTC::InitRTC();
    kpok("RTC INIT!\n");

    uint64_t TSC1 = rdtsc();
    PIT::Sleep(1000);
    kinfo("CPU Hz %lld\n", (rdtsc() - TSC1));

    kinfo("INIT HEAP...\n");

    void* stack = HIGHER_HALF(PMM::Alloc(3) + (3 * PAGE_SIZE));
    tss_list[0].rsp[0] = (u64)stack;

    Heap::Init();
    kpok("KHeap initialised.\n");

    kinfo("INIT ACPI...\n");
    if(!ACPI::Init((void*)RSDP_ADDR)){
        kerror("ACPI INIT FAILED:Couldn't find ACPI.\n");
        hcf();
    }
    kpok("ACPI INIT!\n");
    kinfo("INIT MADT...\n");
    MADT_Init();
    kpok("MADT INIT!\n");
    
    kinfo("INIT LAPIC...\n");
    LAPIC::Init();
    kpok("LAPIC INIT!\n");

    kinfo("INIT IOAPIC...\n");
    IOAPIC::Init();
    kpok("IOAPIC INIT!\n");

    kinfo("INIT FPU...\n");
    if (fpu_init()){
        kerror("FPU INIT FAILED: x86_64 CPU doesn't support FPU.\n");
        hcf();
    }
    kpok("FPU INIT!\n");
    
    kinfo("REGIST SCHEDULE...\n");
    irq_register(0x80 - 32, Schedule::Schedule);
    kpok("SCHEDULE REGISTED!\n");

    kinfo("INIT SMP...\n");
    smp_init();
    kpok("SMP INIT!\n");

    Schedule::Init();
    user_init();
    LAPIC::CalibrateTimer();

    ATA::Init();

    kinfo("INIT PCI...\n");
    PCI::Init();
    kpok("PCI INIT!\n");

    kinfo("INIT DONE!\n");

    kinfo("INIT AHCI...\n");
    AHCI::AHCIDriver();
    kpok("AHCI INIT!\n");
}