#include "../include/syscall.h"

uint64_t syscall(uint64_t num,uint64_t arg1,uint64_t arg2,
    uint64_t arg3,uint64_t arg4,uint64_t arg5,uint64_t arg6){
    uint64_t ret;
    __asm__ volatile ("mov %1, %%rax;"
                      "mov %2, %%rdi;"
                      "mov %3, %%rsi;"
                      "mov %4, %%rdx;"
                      "mov %5, %%r10;"
                      "mov %6, %%r8;"
                      "mov %7, %%r9;"
                      "int $0x80;"
                      : "=r"(ret)
                      : "g"(num), "g"(arg1), "g"(arg2),"g"(arg3),
                      "g"(arg4),"g"(arg5),"g"(arg6)
                      : "rax", "rdi", "rsi","rdx","r10","r8","r9"
                      );

    return ret;
}