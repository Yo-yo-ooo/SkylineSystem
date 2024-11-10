#pragma once

#include "../print/e9print.h"
#include "kprintf.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "kio.h"
#include "types.h"


extern uint64_t hhdm_offset;

#define PAGE_SIZE 4096

#define DIV_ROUND_UP(x, y) (x + (y - 1)) / y
#define ALIGN_UP(x, y)     DIV_ROUND_UP(x, y) * y
#define ALIGN_DOWN(x, y)   (x / y) * y

#define HIGHER_HALF(ptr)   ((void*)((u64)ptr) + hhdm_offset)
#define VIRTUAL(ptr)       HIGHER_HALF(ptr)
#define PHYSICAL(ptr)      ((void*)((u64)ptr) - hhdm_offset)

void Panic(const char* message);
void Panic(bool halt, const char* message);
#define panic Panic
void hcf(void);

void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);


inline void bitmap_set(u8* bitmap, u64 bit) {
    bitmap[bit / 8] |= 1 << (bit % 8);
}

inline void bitmap_clear(u8* bitmap, u64 bit) {
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

inline bool bitmap_get(u8* bitmap, u64 bit) {
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
}

typedef struct {
    bool locked;
} atomic_lock;

void lock(atomic_lock* l);
void unlock(atomic_lock* l);

u64 rdtsc(void);
