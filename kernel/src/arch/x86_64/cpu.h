#pragma once

#include "cpuid.h"

#include "../../klib/klib.h"

#define IA32_GS_MSR 0xC0000101
#define IA32_GS_KERNEL_MSR 0xC0000102

inline u64 read_cpu_gs() {
    return rdmsr(IA32_GS_MSR);
}

inline void write_cpu_gs(u64 value) {
    wrmsr(IA32_GS_MSR, value);
}

inline u64 read_kernel_gs() {
    return rdmsr(IA32_GS_KERNEL_MSR);
}

inline void write_kernel_gs(u64 value) {
    wrmsr(IA32_GS_KERNEL_MSR, value);
}

