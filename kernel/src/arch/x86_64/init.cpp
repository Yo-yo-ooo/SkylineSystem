//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/pinc.h>
#include <drivers/ahci/ahci.h>
#include <drivers/ata/ata.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <drivers/dev/dev.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/blockdev/blockdev.h>
#include <fs/fc.h>
#include <arch/x86_64/simd/simd.h>
#include <mem/heap.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <drivers/framebuffer/fb.h>
#include <klib/renderer/rnd.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/interrupt/idt.h>
#include <mem/pmm.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/schedule/sched.h>
#include <atomic/atomic.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/rtc/rtc.h>
#include <drivers/mouse/x86/ps2mouse.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

uint32_t PrintFSERIAL = 0;

extern cpu_t *bsp_cpu;
extern "C" int32_t file_cache_writeback_callback(
    const uint8_t *key, 
    uint32_t key_len, void *data, size_t data_len
);
extern void sys_sysinfo_init(void);

extern void enable_smep_smap();
void __init x86_64_init(void){
    InitFunc("Serial(Simulater)",Serial::Init());
    WELCOME_X86_64
    kinfo("INIT x86_64 ARCH\n");
    InitFunc("SSE",sse_enable());
    kinfoln("HHDM OFFSET:0x%X",hhdm_offset);

    InitFunc("GDT",GDT::Init(0));
    bsp_cpu->self = bsp_cpu;
    wrmsr(KERNEL_GS_BASE, (uint64_t)bsp_cpu);
    wrmsr(IA32_GS_MSR,(uint64_t)bsp_cpu);
    InitFunc("IDT",idt_init());
    if (fpu_init()){
        kerror("FPU INIT FAILED: x86_64 CPU doesn't support FPU.\n");
        hcf();
    }
    kpok("FPU INIT!\n");
    InitFunc("PMM",PMM::Init());
    InitFunc("VMM",VMM::Init());
    InitFunc("SLAB",SLAB::Init());
    InitFunc("ACPI",ACPI::Init((void*)RSDP_ADDR));
    InitFunc("MADT",MADT_Init());
    //DISABLE PIC
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
    kinfoln("DISABLED PIC!");
    //ENABLE ICMR
    outb(0x22,0x70);
    outb(0x23,0x01);
    kinfoln("ENABLED ICMR!");
    //simd_cpu_init(get_cpu(0));
    InitFunc("LAPIC",LAPIC::Init());
    InitFunc("IOAPIC",IOAPIC::Init());
    InitFunc("PIT & RTC",PIT::InitPIT());
    InitFunc("HPET",HPET::InitHPET());
    bsp_cpu->file_cache = (file_cache_cpu_t*)kmalloc(sizeof(file_cache_cpu_t));
    file_cache_cpu_init(bsp_cpu->file_cache, bsp_cpu->id,file_cache_writeback_callback);
    InitFunc("SMP",smp_init());
    InitFunc("RTC",RTC::InitRTC());
    InitFunc("SIMD Core 0",simd_cpu_init(this_cpu()));
    InitFunc("Intel SMEP & SMAP",enable_smep_smap());
    
    InitFunc("Schedule",Schedule::Init());
    InitCPUThread();
    InitFunc("Syscall",syscall_init());
    

    InitFunc("VsDev",Dev::Init());
    InitFunc("File & MP MAN",InitFFMAN());
    //InitFunc("ATA",ATA::Init());
    if(ACPI::mcfg == NULL){PCI::DoPCIWithoutMCFG();}
    else{InitFunc("PCI",PCI::EnumeratePCI(ACPI::mcfg));}
    InitFunc("AHCI",new AHCI::AHCIDriver(PCI::FindPCIDev(0x01, 0x06, 0x01)));

    InitFunc("PS/2 MOUSE(x86)",ps2_mouse_init());
    InitFunc("KEYBOARD(x86)",keyboard_init());
    

    if(!ext4_kernel_init("sata0","/mp/",0)){hcf();}

    FrameBufferDevice::Init();

    //ext4_fs_test_all();

    Schedule::Install();

    atomic_store_4(&PrintFSERIAL,1,0);
    sys_sysinfo_init();

    proc_t *proc = Schedule::NewProcess(true);

    kinfoln("Creating desktop process...");
    void *R = VMM::Alloc(kernel_pagemap,1000,false);
    VMM::Free(kernel_pagemap,R);
    char *argv[] = {(char*)"Test Main Thread"};
    char *envp[] = {nullptr};
    thread_t *desktop = Schedule::NewThread(proc, 0, 0, 
        "/mp/desktop.elf", 1, argv, envp);  
    proc_t *proc2 = Schedule::NewProcess(true);
    thread_t *desktop2 = Schedule::NewThread(proc2, 0, 1, 
        "/mp/hw.elf", 1, argv, envp);  
    /*proc_t *proc3 = Schedule::NewProcess(true);
    thread_t *desktop3 = Schedule::NewThread(proc3, 0, 1, 
        "/mp/hw2.elf", 1, argv, envp);   */
    kinfoln("desktop PROCESS: %d",proc->id);
    asm volatile("sti");
    LAPIC::IPI(smp_bsp_cpu,SCHED_VEC);
    LAPIC::IPIOthers(smp_bsp_cpu, SCHED_VEC);

    while (true) {
        asm volatile("hlt");
    }
}