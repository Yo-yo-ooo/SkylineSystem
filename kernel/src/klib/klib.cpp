#include "klib/klib.h"
#include <stdint.h>
#include <stddef.h>

void Panic(const char* message){
    e9_print("Panic!");
    e9_printf(message);
    hcf();
}


// Halt and catch fire function.
void hcf(void) {
    for (;;) {
#if defined (__x86_64__)
        asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm ("wfi");
#elif defined (__loongarch64)
        asm ("idle 0");
#endif
    }
}