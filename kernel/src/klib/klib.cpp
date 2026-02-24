/*
* SPDX-License-Identifier: GPL-2.0-only
* File: klib.cpp
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
#include <klib/klib.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __x86_64__
#include <arch/x86_64/atomic/atomic_arch.h>
#endif

void Panic(const char* message){
    kerrorln("Panic!");
    e9_printf(message);
    hcf();
}

void Panic(bool halt, const char* message){
    e9_print("Panic!");
    e9_printf(message);
    if(halt){
        hcf();
    }
}

void Panic(const char* message,bool halt){
    e9_print("Panic!");
    e9_printf(message);
    if(halt){
        hcf();
    }
}

// Halt and catch fire function.
void hcf(void) {
    for (;;) {
#ifdef __x86_64__
        asm volatile("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm volatile("wfi");
#elif defined (__loongarch64)
        asm volatile("idle 0");
#endif
    }
}



void spinlock_lock(spinlock_t* l) {
    
#ifdef __x86_64__
    while(__a_swap(l,1) != 0)
        __a_spin();
#else
    while (__sync_lock_test_and_set(l, 1)){
        asm volatile("nop");
    } 
#endif
}

void spinlock_unlock(spinlock_t* l) {
    //__sync_lock_release(l);
    
#ifdef __x86_64__
    __a_store(l,0);
#else
    __sync_lock_release(l);
#endif
}

extern "C" void *__memcpy(void * d, const void * s, uint64_t n) { 
    _memcpy(s, d, n);
    return d;
}

void bitmap_set(u8* bitmap, u64 bit) {
#if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    bitmap[bit / 8] |= 1 << (bit % 8);
#elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    bitmap[bit / 8] = __builtin_bswap64(bitmap[bit/8]);
    bitmap[bit / 8] |= 1 << (bit % 8);
    bitmap[bit / 8] = __builtin_bswap64(bitmap[bit/8]);
#else
#error "Unknown endianness"
#endif
}

void bitmap_clear(u8* bitmap, u64 bit) {
#if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    bitmap[bit / 8] &= ~(1 << (bit % 8));
#elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    bitmap[bit / 8] = __builtin_bswap64(bitmap[bit / 8] & (~(1 << (bit % 8))));
#else
#error "Unknown endianness"
#endif
}


bool bitmap_get(u8* bitmap, u64 bit) {
#if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
#elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    return (bitmap[bit / 8] & (1 >> (bit % 8))) != 0;
#else
#error "Unknown endianness"
#endif
}


uint16_t kld_16 (const uint8_t* ptr)	/*	 Load a 2-byte little-endian word */
{
	uint16_t rv;

	rv = ptr[1];
	rv = rv << 8 | ptr[0];
	return rv;
}

uint32_t kld_32 (const uint8_t* ptr)	/* Load a 4-byte little-endian word */
{
	uint32_t rv;

	rv = ptr[3];
	rv = rv << 8 | ptr[2];
	rv = rv << 8 | ptr[1];
	rv = rv << 8 | ptr[0];
	return rv;
}

uint64_t kld_64 (const uint8_t* ptr)	/* Load an 8-byte little-endian word */
{
	uint64_t rv;

	rv = ptr[7];
	rv = rv << 8 | ptr[6];
	rv = rv << 8 | ptr[5];
	rv = rv << 8 | ptr[4];
	rv = rv << 8 | ptr[3];
	rv = rv << 8 | ptr[2];
	rv = rv << 8 | ptr[1];
	rv = rv << 8 | ptr[0];
	return rv;
}

#if defined(__x86_64__)
#include <arch/x86_64/rtc/rtc.h>
#endif

extern "C" uint32_t sys_now(void){
#if defined(__x86_64__)
    return (uint32_t)(RTC::Year - 80) << 25 |
           (uint32_t)(RTC::Month + 1) << 21 |
           (uint32_t)RTC::Day << 16 |
           (uint32_t)RTC::Hour << 11 |
           (uint32_t)RTC::Minute << 5 |
           (uint32_t)RTC::Second >> 1;
#else
    return 0;
#endif

}

volatile int KernelInited = 0;
int8_t *KernelXsaveSpace = nullptr;