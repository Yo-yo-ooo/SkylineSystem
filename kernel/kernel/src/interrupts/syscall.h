#pragma once
#include <stdint.h>

using usz = uintptr_t;

constexpr usz NUM_SYSCALLS = 25;

extern void *syscalls[NUM_SYSCALLS];
extern usz num_syscalls;

extern "C" void system_call_handler_asm();