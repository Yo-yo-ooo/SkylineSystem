#include "syscall.h"
#include "../kernelStuff/memory/memory.h"

uint64_t num_syscalls = NUM_SYSCALLS;

int test_call(){
    return 1;
}

void *syscalls[NUM_SYSCALLS] = {
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
    (void*)test_call,
};