#include <stdint.h>

using usz = uintptr_t;

constexpr usz NUM_SYSCALLS = 2;
extern void* syscalls[NUM_SYSCALLS];

// Defined in `syscalls.cpp`
// Used by `syscalls.asm`
extern usz num_syscalls;

// Defined in `syscalls.asm`
extern "C" void system_call_handler_asm();