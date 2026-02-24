/*
* SPDX-License-Identifier: GPL-2.0-only
* File: klib.h
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#pragma once

#ifndef _KLIB_H_
#define _KLIB_H_

#include <print/e9print.h>
#include <klib/kprintf.h>
#include <conf.h>

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <pdef.h>
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

#define HIGHER_HALF(x)   (decltype(x))((uint64_t)x + hhdm_offset)
#define VIRTUAL(ptr)       HIGHER_HALF(ptr)
#define PHYSICAL(x)      (decltype(x))((uint64_t)x - hhdm_offset)
#define FORCE_INLINE inline __attribute__((always_inline))

void Panic(const char* message);
void Panic(bool halt, const char* message);
void Panic(const char* message,bool halt);
#define panic Panic
void hcf(void);


#define ASSERT(CONDITION) \
if (CONDITION){}else {Panic(#CONDITION);}
extern "C" {
void _memcpy(void* src, void* dest, uint64_t size);
void _memset(void* dest, uint8_t value, uint64_t size);
void _memmove(void* dest,void* src, uint64_t size);
int32_t _memcmp(const void* buffer1,const void* buffer2,size_t  size);
void *__memcpy(void * d, const void * s, uint64_t n);
}

void bitmap_set(u8* bitmap, u64 bit);
void bitmap_clear(u8* bitmap, u64 bit);
bool bitmap_get(u8* bitmap, u64 bit);

void spinlock_lock(spinlock_t* l);
void spinlock_unlock(spinlock_t* l);

//class func pointer -> func pointer
template <typename T>
T CFCast(auto F){
	union FT
	{T   t;decltype(F) f;};
	FT ft;ft.f=F;
	return ft.t;  //此为技巧，利用联合将地址返回。
}

#define __init
#ifdef __x86_64__
#define __ffunc __attribute__((target("sse2")))
#define _intr __attribute__((interrupt))
extern uint32_t MaxXsaveSize;
extern int8_t *KernelXsaveSpace;
#else
#define __ffunc
#endif
#define UnCompleteCode 1

extern volatile int KernelInited;

uint16_t kld_16 (const uint8_t* ptr);
uint32_t kld_32 (const uint8_t* ptr);
uint64_t kld_64 (const uint8_t* ptr);

void qsort(void *base, size_t num, size_t width, int32_t (*sort)(const void *e1, const void *e2));


namespace Interrupt{
    // 返回 1 表示中断开启，0 表示中断关闭
    static inline int32_t State() {
    #ifdef __x86_64__
        u64 rflags; 
        __asm__ volatile (
            "pushfq     \n\t" 
            "popq %0    \n\t" 
            : "=r"(rflags) 
            : 
            : "memory" 
        ); 
        return (rflags >> 9) & 1; 
    #elif defined(__aarch64__)
        uint64_t daif;
        asm volatile("mrs %0, daif" : "=r"(daif));
        // 检查第 7 位 (IRQ)，注意：DAIF 位为 1 是屏蔽，所以取反
        return !(daif & (1 << 7));
    #else defined(__riscv)
        uint64_t sstatus;
        asm volatile("csrr %0, sstatus" : "=r"(sstatus));
        // 检查第 1 位 (SIE)
        return (sstatus & (1 << 1)) ? 1 : 0;
    #endif
    }

    // 屏蔽 IRQ (DAIF 中的 I 位)
    static inline void Mask() {
    #ifdef __x86_64__
        asm volatile("cli" ::: "memory");
    #elif defined(__aarch64__)
        // #2 对应 DAIF 寄存器的 I 位 (bit 7)
        // 使用 msr 指令直接置位
        asm volatile("msr daifset, #2" ::: "memory");
    #elif defined(__riscv)
        // csrci 指令：CSR Clear Immediate (将立即数对应的位清零)
        // 2 对应 sstatus 寄存器的 SIE 位 (bit 1)
        asm volatile("csrci sstatus, 2" ::: "memory");
    #endif
    }

    // 开启 IRQ
    static inline void Unmask() {
    #ifdef __x86_64__
        asm volatile ("sti" ::: "memory");
    #elif defined(__aarch64__)
        // 使用 msr 指令直接清除 I 位
        asm volatile("msr daifclr, #2" ::: "memory");
    #elif defined(__riscv)
        // csrsi 指令：CSR Set Immediate (将立即数对应的位设置为 1)
        asm volatile("csrsi sstatus, 2" ::: "memory");
    #endif
        
    }
}


extern "C" {
//freestanding cpu features functions
func_optimize(3) void *memset_fscpuf(void *dst, const int32_t val, size_t n);
func_optimize(3) void *memcpy_fscpuf(void *dst, const void *src, size_t n);
func_optimize(3) void *memmove_fscpuf(void *dst, const void *src, size_t n);
func_optimize(3) int32_t memcmp_fscpuf(const void *left, const void *right, size_t len);
func_optimize(3) void bzero(void *dst, size_t n);
}

#endif