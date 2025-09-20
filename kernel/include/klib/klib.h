#pragma once

#ifndef _KLIB_H_
#define _KLIB_H_

#include <print/e9print.h>
#include <klib/kprintf.h>
#include <conf.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
//#include "kio.h"
#include <klib/types.h>
#include <klib/list.h>
#include <klib/cstr.h>
#include <klib/utf8.h>
#include <klib/fifo.h>
#include <klib/serial.h>
#include <klib/ctype.h>

extern volatile uint64_t hhdm_offset;
extern volatile uint64_t RSDP_ADDR;

#define PAGE_SIZE 4096

#define DIV_ROUND_UP(x, y) (x + (y - 1)) / y
#define ALIGN_UP(x, y)     DIV_ROUND_UP(x, y) * y
#define ALIGN_DOWN(x, y)   ((x / y) * y)

#define HIGHER_HALF(x)   (typeof(x))((uint64_t)x + hhdm_offset)
#define VIRTUAL(ptr)       HIGHER_HALF(ptr)
#define PHYSICAL(x)      (typeof(x))((uint64_t)x - hhdm_offset)
#define FORCE_INLINE inline __attribute__((always_inline))

void Panic(const char* message);
void Panic(bool halt, const char* message);
void Panic(const char* message,bool halt);
#define panic Panic
void hcf(void);


#define ASSERT(CONDITION) \
if (CONDITION){}else {Panic(#CONDITION);}

void _memcpy(void* src, void* dest, uint64_t size);
void _memset(void* dest, uint8_t value, uint64_t size);
void _memmove(void* src, void* dest, uint64_t size);
int32_t _memcmp(const void* buffer1,const void* buffer2,size_t  count);
void *__memcpy(void * d, const void * s, uint64_t n);

void bitmap_set(u8* bitmap, u64 bit);
void bitmap_clear(u8* bitmap, u64 bit);
bool bitmap_get(u8* bitmap, u64 bit);

void spinlock_lock(spinlock_t* l);
void spinlock_unlock(spinlock_t* l);

//class func pointer -> func pointer
template <typename T>
T CFCast(auto F)
{
	union FT
	{T   t;decltype(F) f;};
	FT ft;ft.f=F;
	return ft.t;  //此为技巧，利用联合将地址返回。
}

#define __init
#ifdef __x86_64__
#define __ffunc __attribute__((target("sse2")))
#define _intr __attribute__((interrupt))
#else
#define __ffunc
#endif
#define UnCompleteCode 1

uint16_t kld_16 (const uint8_t* ptr);
uint32_t kld_32 (const uint8_t* ptr);
uint64_t kld_64 (const uint8_t* ptr);

void qsort(void *base, size_t num, size_t width, int32_t (*sort)(const void *e1, const void *e2));

#define max(a, b) ({ \
    __typeof__(a) ta = (a); \
    __typeof__(b) tb = (b); \
    ta < tb ? tb : ta; \
})

#define min(a, b) ({ \
    __typeof__(a) ta = (a); \
    __typeof__(b) tb = (b); \
    ta > tb ? tb : ta; \
})

#endif