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


#define ASSERT(CONDITION) \
if (CONDITION){} \
/* 符号#让编译器将宏的参数转化为字符串字面量 */ \
else {Panic(#CONDITION);}

void _memcpy(void* src, void* dest, uint64_t size);
void _memset(void* dest, uint8_t value, uint64_t size);
void _memmove(void* src, void* dest, uint64_t size);
int _memcmp(const void* buffer1,const void* buffer2,size_t  count);
void *__memcpy(void *__restrict__ d, const void *__restrict__ s, size_t n);
void *_EXT4_T_memcpy(void *__restrict__ dest,   const void * __restrict__ src, size_t n);

void bitmap_set(u8* bitmap, u64 bit);
void bitmap_clear(u8* bitmap, u64 bit);
bool bitmap_get(u8* bitmap, u64 bit);

void lock(atomic_lock* l);
void unlock(atomic_lock* l);

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
#include <arch/x86_64/MStack/MStackM.h>
#include <arch/x86_64/MStack/MStackS.h>
#else
#define __ffunc
#endif
#define UnCompleteCode 1

uint16_t kld_16 (const uint8_t* ptr);
uint32_t kld_32 (const uint8_t* ptr);
uint64_t kld_64 (const uint8_t* ptr);

void qsort(void *base, size_t num, size_t width, int32_t (*sort)(const void *e1, const void *e2));

#endif