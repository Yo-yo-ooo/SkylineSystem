#include "klib/klib.h"
#include <stdint.h>
#include <stddef.h>

void Panic(const char* message){
    e9_print("Panic!");
    e9_printf(message);
    hcf();
}

void Panic(bool halt, const char* message){
    e9_print("Panic!");
    e9_printf(message);
    if(halt){
        hcf();
    }
}

// Halt and catch fire function.
void hcf(void) {
    for (;;) {
#ifdef __x86_64__
        asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm ("wfi");
#elif defined (__loongarch64)
        asm ("idle 0");
#endif
    }
}



void lock(atomic_lock* l) {
    while (__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__)
        __asm__ volatile("pause");
#endif
    }
}

void unlock(atomic_lock* l) {
    __atomic_clear(&l->locked, __ATOMIC_RELEASE);
}