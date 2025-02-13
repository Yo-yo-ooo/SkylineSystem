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
    InitFunc("PCI",PCI::EnumeratePCI(ACPI::mcfg));
    InitFunc("AHCI",new AHCI::AHCIDriver(PCI::FindPCIDev(0x01, 0x06, 0x01)));

    InitFunc("Dev",Dev::Init());
    InitFunc("KEYBOARD(x86)",keyboard_init());

    uint32_t r = ext4_device_register(ext4_blockdev_get(0), "sata0");
	if (r != EOK) {
		kerror("ext4_device_register: rc = %d\n", r);
		//return;
	}

	r = ext4_mount("sata0", "/mp/", false);
	if (r != EOK){kerror("ext4_mount: rc = %d\n", r);hcf();}
    kpok("EXT4 MOUNTED!\n");
    

	r = ext4_recover("/mp/");
	if (r != EOK && r != ENOTSUP) {
		kerror("ext4_recover: rc = %d\n", r);
		//return;
	}
    hcf();

	r = ext4_journal_start("/mp/");
	if (r != EOK) {
		kerror("ext4_journal_start: rc = %d\n", r);
		//return;
	}
    hcf();

	ext4_cache_write_back("/mp/", 1);
    ext4_file* f;
    ext4_fopen(f,"/mp/test.txt", "w+");
    ext4_fclose(f);
}