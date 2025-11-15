#include <arch/x86_64/allin.h>
#include <arch/pinc.h>
#include <drivers/ahci/ahci.h>
#include <drivers/ata/ata.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <drivers/dev/dev.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/blockdev/blockdev.h>

#include <arch/x86_64/simd/simd.h>
#include <mem/heap.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

extern void FT_Deinit();
#define InitFunc(name,func) /* kinfo("Initialise %s...\n",name); */func;kpok("%s Initialised!\n",name)

void __init x86_64_init(void){
    InitFunc("Serial(Simulater)",Serial::Init());
    WELCOME_X86_64
    kinfo("INIT x86_64 ARCH\n");
    kinfoln("HHDM OFFSET:0x%X",hhdm_offset);

    InitFunc("GDT",GDT::Init(0));
    InitFunc("IDT",idt_init());
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
    simd_cpu_init(get_cpu(0));
    InitFunc("LAPIC",LAPIC::Init());
    InitFunc("IOAPIC",IOAPIC::Init());
    InitFunc("PIT",PIT::InitPIT());
    InitFunc("SMP",smp_init());
    InitFunc("RTC",RTC::InitRTC());
    
    
    if (fpu_init()){
        kerror("FPU INIT FAILED: x86_64 CPU doesn't support FPU.\n");
        hcf();
    }
    kpok("FPU INIT!\n");
    InitFunc("SSE",sse_enable());
    InitFunc("Schedule",Schedule::Init());
    InitFunc("Syscall",syscall_init());
    

    InitFunc("VsDev",Dev::Init());
    //InitFunc("ATA",ATA::Init());
    if(ACPI::mcfg == NULL){PCI::DoPCIWithoutMCFG();}
    else{InitFunc("PCI",PCI::EnumeratePCI(ACPI::mcfg));}
    InitFunc("AHCI",new AHCI::AHCIDriver(PCI::FindPCIDev(0x01, 0x06, 0x01)));

    InitFunc("KEYBOARD(x86)",keyboard_init());

    if(!ext4_kernel_init("sata0","/mp/")){hcf();}

    Schedule::Install();

    proc_t *proc = Schedule::NewProcess(true);

    
    thread_t *desktop = Schedule::NewThread(proc, 0, 0, 
        "/mp/bin/desktop.elf", 1, (char*[]){"Desktop Main Thread"}, (char*[]){nullptr}); 
    kinfoln("Desktop Thread: %d",desktop->id);

    LAPIC::IPIOthers(0, SCHED_VEC);
}