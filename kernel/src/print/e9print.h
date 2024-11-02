#pragma once

#include <stdarg.h>
#include <stddef.h>

void e9_putc(char c);
void e9_print(const char *msg);
void e9_puts(const char *msg);
void e9_printf(const char *format, ...);

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