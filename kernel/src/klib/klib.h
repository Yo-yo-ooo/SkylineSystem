#pragma once

#include "../print/e9print.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef uintptr_t uptr;

extern uint64_t hhdm_offset;

#define PAGE_SIZE 4096

#define DIV_ROUND_UP(x, y) (x + (y - 1)) / y
#define ALIGN_UP(x, y)     DIV_ROUND_UP(x, y) * y
#define ALIGN_DOWN(x, y)   (x / y) * y

#define HIGHER_HALF(ptr)   ((void*)((u64)ptr) + hhdm_offset)
#define VIRTUAL(ptr)       HIGHER_HALF(ptr)
#define PHYSICAL(ptr)      ((void*)((u64)ptr) - hhdm_offset)

void Panic(const char* message);
void hcf(void);

