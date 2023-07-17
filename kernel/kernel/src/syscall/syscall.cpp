#include "syscall.h"

uint64_t num_syscalls = OS_NUM_SYSCALLS;
void* syscalls[OS_NUM_SYSCALLS] = {
    (void*)0,
}