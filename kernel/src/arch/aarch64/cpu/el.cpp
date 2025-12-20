#include <arch/aarch64/cpu/features.h>

uint8_t GetCurrentEL(){
    uint64_t result = 0;
    asm volatile ("mrs %0, CurrentEL " : "=r"(result));

    switch (result){
    case 0xC:
        return 3;
    case 0x8:
        return 2;
    case 0x4:
        return 1;
    case 0x0:
        return 0;
    default:
        return -1;
    }
    return -1;
}