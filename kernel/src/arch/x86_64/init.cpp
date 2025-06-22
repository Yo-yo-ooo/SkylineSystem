#include <arch/x86_64/allin.h>
#include <arch/pinc.h>
#include <drivers/ahci/ahci.h>
#include <drivers/ata/ata.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <drivers/vsdev/vsdev.h>
#include <fs/vfs.h>
#include <drivers/dev/dev.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/blockdev/blockdev.h>

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
    for (int i = 0; i < 1000; i++)
        MStackData::stackArr[i] = MStack();
    MStackData::BenchmarkEnabled = false;

    InitFunc("SSE",sse_enable());
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
    InitFunc("VsDev",VsDev::Init());
    //InitFunc("ATA",ATA::Init());
    if(ACPI::mcfg == NULL)
        PCI::DoPCIWithoutMCFG();
    InitFunc("PCI",PCI::EnumeratePCI(ACPI::mcfg));
    InitFunc("AHCI",new AHCI::AHCIDriver(PCI::FindPCIDev(0x01, 0x06, 0x01)));

    InitFunc("Dev",Dev::Init());
    InitFunc("KEYBOARD(x86)",keyboard_init());

    if(!ext4_kernel_init("sata0","/mp/")){hcf();}

	test_lwext4_dir_ls("/mp/");

    ext4_file f;
    int r;
    size_t* a;
    r = ext4_fopen(&f,"/mp/test.txt", "wb+");
    if (r != EOK) {
		kinfoln("ext4_fopen ERROR = %d\n", r);
		return false;
	}
    test_lwext4_dir_ls("/mp/");
    char nbuf[13] = "Hello World";
    size_t strs = strlen(nbuf);
    r = ext4_fwrite(&f,nbuf,strs,a);
    kinfoln("a:%d",a); 
    kinfoln("r:%d",r);
    r = ext4_fclose(&f);
   
    ext4_file f2;
    r = ext4_fopen(&f2, "/mp/test.txt", "r");
	if (r != EOK) {
		kinfoln("ext4_fopen ERROR = %d\n", r);
		return false;
	}
    
    kinfoln("%s",nbuf);
    _memset(nbuf,0,13);
    r = ext4_fread(&f2,nbuf,strs,a);
    kinfoln("r:%d",r);
    kinfoln("nbuf:%s",nbuf);
    r = ext4_fclose(&f2);
}