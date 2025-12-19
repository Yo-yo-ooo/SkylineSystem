#include <klib/kprintf.h>
#include <arch/pinc.h>
#include <arch/aarch64/cpu/features.h>
#include <mem/pmm.h>
#include <arch/aarch64/vmm/vmm.h>


void aarch64_init(void){
    WELCOME_AARCH64
    CollectCPUFeatures();
    kpokln("Collected CPU Features in aarch64 architecture.!");
    InitFunc("PMM",PMM::Init());
    InitFunc("VMM",VMM::Init());
}