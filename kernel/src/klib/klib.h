#pragma once

#ifndef _KLIB_H_
#define _KLIB_H_

#include "../print/e9print.h"
#include "kprintf.h"


#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kio.h"
#include "types.h"
#include "list.h"
#include "cstr.h"

extern uint64_t hhdm_offset;
extern uint64_t RSDP_ADDR;

#define PAGE_SIZE 4096

#define DIV_ROUND_UP(x, y) (x + (y - 1)) / y
#define ALIGN_UP(x, y)     DIV_ROUND_UP(x, y) * y
#define ALIGN_DOWN(x, y)   (x / y) * y

#define HIGHER_HALF(ptr)   ((void*)((u64)ptr) + hhdm_offset)
#define VIRTUAL(ptr)       HIGHER_HALF(ptr)
#define PHYSICAL(ptr)      ((void*)((u64)ptr) - hhdm_offset)

void Panic(const char* message);
void Panic(bool halt, const char* message);
void Panic(const char* message,bool halt);
#define panic Panic
void hcf(void);

void _memcpy(void* src, void* dest, uint64_t size);
void _memset(void* dest, uint8_t value, uint64_t size);
void _memmove(void* src, void* dest, uint64_t size);
int _memcmp(const void* buffer1,const void* buffer2,size_t  count);


inline void bitmap_set(u8* bitmap, u64 bit) {
#if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    bitmap[bit / 8] |= 1 << (bit % 8);
#elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    bitmap[bit / 8] = __builtin_bswap64(bitmap[bit / 8] | (1 << (bit % 8)));
#else
#error "Unknown endianness"
#endif
}

inline void bitmap_clear(u8* bitmap, u64 bit) {
#if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    bitmap[bit / 8] &= ~(1 << (bit % 8));
#elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    bitmap[bit / 8] = __builtin_bswap64(bitmap[bit / 8] & (~(1 << (bit % 8))));
#else
#error "Unknown endianness"
#endif
}

inline bool bitmap_get(u8* bitmap, u64 bit) {
#if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
#elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    return __builtin_bswap64(bitmap[bit / 8] & (1 << (bit % 8))) != 0;
#else
#error "Unknown endianness"
#endif
}


void lock(atomic_lock* l);
void unlock(atomic_lock* l);


#define __init
#ifdef __x86_64__
#define __ffunc __attribute__((target("sse2")))
#else
#define __ffunc
#endif

inline int strlen(const char* str) {
    int i = 0;
    while (*str != '\0') {
        i++;
        str++;
    }
    return i;
}

#endif