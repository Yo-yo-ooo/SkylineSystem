#include <klib/kprintf.h>
#include <arch/pinc.h>
#include <arch/aarch64/cpu/features.h>



void aarch64_init(void){
    WELCOME_AARCH64
    CollectCPUFeatures();
    kpokln("Collected CPU Features in aarch64 architecture.!");
    
}