#pragma once
#ifndef x86MEM_ENABLE_H
#define x86MEM_ENABLE_H

#include <stdint.h>
#include <stddef.h>
#include <klib/kio.h>

#ifdef __x86_64__

typedef struct smid_x86_64
{
    uint8_t* buffer;
} simd_ctx_t;

void simd_cpu_init(void);

uint64_t simd_ctx_init(simd_ctx_t* ctx);

void simd_ctx_deinit(simd_ctx_t* ctx);

void simd_ctx_save(simd_ctx_t* ctx);

void simd_ctx_load(simd_ctx_t* ctx);

#else
#error "You include x86_64 ARCH file,but your ARCH not x86_64"
#endif


#endif