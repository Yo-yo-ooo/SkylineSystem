#pragma once
#ifndef x86MEM_ENABLE_H
#define x86MEM_ENABLE_H

#include <stdint.h>
#include <stddef.h>
#include <klib/kio.h>

extern "C++" {

#ifdef __x86_64__


void simd_cpu_init(cpu_t *cpu);

#else
#error "You include x86_64 ARCH file,but your ARCH not x86_64"
#endif

}

#endif