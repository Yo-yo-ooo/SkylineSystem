#pragma once
#ifndef _x86_64_ASM_XCR_H
#define _x86_64_ASM_XCR_H

#include <stdint.h>
#include <stddef.h>
#include <klib/kio.h>
#include <pdef.h>


constexpr uint8_t XCR_XSTATE_FEATURES_ENABLED = 0;
constexpr uint8_t XCR_XSTATE_FEATURES_IN_USE = 1;


static inline uint64_t read_xcr(const uint8_t xcr) {
    uint32_t eax = 0;
    uint32_t edx = 0;

    asm volatile ("xgetbv" : "=a"(eax), "=d"(edx) : "c"(xcr));
    return (uint64_t)edx << 32 | eax;
}

static inline void write_xcr(const uint8_t xcr, const uint64_t val) {
    asm volatile ("xsetbv"
                  :: "a"((uint32_t)val), "c"(xcr), "d"((uint32_t)(val >> 32))
                  : "memory");
}

#endif