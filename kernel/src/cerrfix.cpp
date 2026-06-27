//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
//These function just for compile (idk why)
#include <stdint.h>
extern "C" void _Unwind_Resume(void){/*unrealized!*/}
extern "C" void __cxa_throw_bad_array_new_length(void) {}
extern "C" int32_t __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
extern "C" void __cxa_pure_virtual() { 
    for (;;) {
#ifdef __x86_64__
        asm volatile("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm volatile("wfi");
#elif defined (__loongarch64)
        asm volatile("idle 0");
#endif
    } 
}
extern "C"{void *__dso_handle;}

