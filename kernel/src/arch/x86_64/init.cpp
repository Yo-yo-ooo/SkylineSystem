#include <arch/x86_64/allin.h>
#include <arch/pinc.h>
#include <drivers/ahci/ahci.h>
#include <drivers/ata/ata.h>
#include <drivers/keyboard/x86/keyboard.h>
#include <drivers/dev/dev.h>
#include <fs/vfs.h>
#include <fs/lwext4/ctvfs.h>
#include <fs/lwext4/ext4.h>
#include <fs/lwext4/blockdev/blockdev.h>

#include <klib/x86/enable.h>
#include <klib/x86/memcpy.h>
#include <mem/heap.h>

#define PIC1_COMMAND 0x20
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1


#define InitFunc(name,func) kinfo("Initialise %s...\n",name);func;kpok("%s Initialised!\n",name)

void __init x86_64_init(void){
    InitFunc("Serial(Simulater)",Serial::Init());
    WELCOME_X86_64
    kinfo("INIT x86_64 ARCH\n");

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
    InitFunc("LAPIC",LAPIC::Init());
    InitFunc("IOAPIC",IOAPIC::Init());
    InitFunc("PIT",PIT::InitPIT());
    InitFunc("SMP",smp_init());
    InitFunc("RTC",RTC::InitRTC());
    
    kinfo("INIT FPU...\n");
    if (fpu_init()){
        kerror("FPU INIT FAILED: x86_64 CPU doesn't support FPU.\n");
        hcf();
    }
    kpok("FPU INIT!\n");
    //InitFunc("XSTATE",xstate_enable());
    InitFunc("SSE",sse_enable());
    //InitFunc("AVX",avx_enable());
    //InitFunc("AVX512",avx512_enable());

    InitFunc("VFS",VFS::Init());
    InitFunc("VsDev",Dev::Init());
    //InitFunc("ATA",ATA::Init());
    if(ACPI::mcfg == NULL){PCI::DoPCIWithoutMCFG();}
    else{InitFunc("PCI",PCI::EnumeratePCI(ACPI::mcfg));}
    InitFunc("AHCI",new AHCI::AHCIDriver(PCI::FindPCIDev(0x01, 0x06, 0x01)));

    InitFunc("KEYBOARD(x86)",keyboard_init());

    EXT4_VFS::Init("sata0","/mp/",0);

    /* if(!ext4_kernel_init("sata0","/mp/")){hcf();}*/

	test_lwext4_dir_ls("/mp/");

    /*uint8_t buf[12] = "Hello WORLD";
    if(test_lwext4_file_test(buf,strlen(buf),2) == true)
        kprintf("[Ext4 Test?]YESSSSSSSSSSSSSSSSS\n");

    if(FSAllIdentify() == false)
        kerror("False!");
    kinfoln("I");
    FSPrintDesc(); */
}