#pragma once
#ifndef x86_XSTATE_H
#define x86_XSTATE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __x86_64__

#define XSAVE_X87 1
#define XSAVE_SSE 2
#define XSAVE_AVX 4
#define XSAVE_AVX512 8
#define XSAVE_ALL 15

struct xstate {
    uint8_t reserved[0];
} __attribute__((aligned((64)))); 

struct xstate_buffer {
    struct xstate* begin;
    uint8_t buffer[0];
};

void xsave(int32_t what, struct xstate_buffer* to);
void xrstor(int32_t what, struct xstate_buffer* from);

#else
#error "You include x86_64 ARCH file,but your ARCH not x86_64"
#endif

#endif