#pragma once

#include <arch/x86_64/schedule/sched.h>

typedef struct {
    void* arg1;
    void* arg2;
    void* arg3;
    void* arg4;
    void* arg5;
    void* arg6;
    registers* r;
} syscall_args;

#define IA32_EFER  0xC0000080
#define IA32_STAR  0xC0000081
#define IA32_LSTAR 0xC0000082
#define IA32_CSTAR 0xC0000083

void user_init();
extern "C" void syscall_handle(registers* r);
