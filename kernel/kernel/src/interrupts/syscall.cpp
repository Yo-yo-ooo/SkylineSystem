#include "syscall.h"

void test_call(){
    return;
}

uint64_t num_syscalls = NUM_SYSCALLS;
void* syscalls[NUM_SYSCALLS] = {
    (void*)test_call
};
