#pragma once
#ifndef ARCH_AARCH64_ASM_MAIR_H
#define ARCH_AARCH64_ASM_MAIR_H

#include <stdint.h>

static inline uint64_t read_mair_el1() {
    uint64_t value = 0;
    asm volatile ("mrs %0, mair_el1" : "=r"(value));

    return value;
}

static inline void write_mair_el1(const uint64_t value) {
    asm volatile ("msr mair_el1, %0" :: "r"(value));
}

#endif