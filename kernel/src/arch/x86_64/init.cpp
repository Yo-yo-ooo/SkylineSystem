#include <arch/x86_64/allin.h>
#include <arch/pinc.h>
#include <drivers/ahci/ahci.h>
#include <drivers/ata/ata.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <drivers/dev/dev.h>
#include <fs/vfs.h>
//#include <drivers/dev/dev.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/blockdev/blockdev.h>

#include <klib/x86/enable.h>
#include <klib/x86/memcpy.h>


#define InitFunc(name,func) kinfo("INIT %s...\n",name);func;kpok("%s INIT!\n",name)

void __init x86_64_init(void){
    Serial::Init();
    WELCOME_X86_64
    kinfo("INIT x86_64 ARCH\n");

    InitFunc("GDT",gdt_init());
    InitFunc("IDT",idt_init());
    InitFunc("PMM",PMM::Init());
    InitFunc("VMM",VMM::Init());

    MStackData::stackPointer = 0;
    for (int32_t i = 0; i < 1000; i++)
        MStackData::stackArr[i] = MStack();
    MStackData::BenchmarkEnabled = false;

    //InitFunc("XSTATE",xstate_enable());

    InitFunc("SSE",sse_enable());

    //InitFunc("AVX",avx_enable());
    //InitFunc("AVX512",avx512_enable());

    InitFunc("PIT",PIT::InitPIT());
    InitFunc("RTC",RTC::InitRTC());

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
    InitFunc("MADT",MADT_Init());
    InitFunc("LAPIC",LAPIC::Init());
    InitFunc("IOAPIC",IOAPIC::Init());

    kinfo("INIT FPU...\n");
    if (fpu_init()){
        kerror("FPU INIT FAILED: x86_64 CPU doesn't support FPU.\n");
        hcf();
    }
    kpok("FPU INIT!\n");
    
    kinfo("REGIST SCHEDULE...\n");
    irq_register(0x80 - 32, Schedule::Schedule);
    kpok("SCHEDULE REGISTED!\n");
    InitFunc("SMP",smp_init());

    Schedule::Init();
    user_init();
    LAPIC::CalibrateTimer();
    InitFunc("VsDev",Dev::Init());
    //InitFunc("ATA",ATA::Init());
    if(ACPI::mcfg == NULL)
        PCI::DoPCIWithoutMCFG();
    InitFunc("PCI",PCI::EnumeratePCI(ACPI::mcfg));
    InitFunc("AHCI",new AHCI::AHCIDriver(PCI::FindPCIDev(0x01, 0x06, 0x01)));

    //InitFunc("Dev",Dev::Init());
    InitFunc("KEYBOARD(x86)",keyboard_init());

    if(!ext4_kernel_init("sata0","/mp/")){hcf();}

	test_lwext4_dir_ls("/mp/");

    uint8_t buf[12] = "Hello WORLD";
    if(test_lwext4_file_test(buf,strlen(buf),2) == true)
        kpok("[Ext4 Test?]YESSSSSSSSSSSSSSSSS\n");

    if(FSAllIdentify() == false)
        kerror("False!");
    kinfoln("I");
    FSPrintDesc();
}