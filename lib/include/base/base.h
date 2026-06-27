//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#pragma once
#ifndef BASE_H_
#define BASE_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

//IO RDTSC
static inline uint64_t rdtsc(void) {
#if defined(__x86_64__)
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t)hi << 32) | lo;
#elif defined(__aarch64__)
    uint64_t val;
    __asm__ volatile ("mrs %0, cntvct_el0" : "=r" (val));
    return val;
#elif defined(__riscv)
    uint64_t val;
    __asm__ volatile ("rdtime %0" : "=r" (val));
    return val;
#else
    #error "Architecture not supported"
#endif
}

#ifdef __cplusplus
}
#endif


#endif