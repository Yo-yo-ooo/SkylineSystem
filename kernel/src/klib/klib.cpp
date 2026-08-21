//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <klib/klib.h>
#include <stdint.h>
#include <stddef.h>
#include <atomic/atomic.h>

extern "C" void Panic(const char* message){
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
    for (;;) {
        if (!__atomic_load_n(l, __ATOMIC_RELAXED) &&
            !__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
            return;
        uint64_t spins = 0;
        while (__atomic_load_n(l, __ATOMIC_RELAXED)) {
            __asm__ __volatile__("pause" ::: "memory");
            if (++spins >= 100000000ULL) {   // 纯自旋 ~1 秒量级，正常竞争不会触发
                kerrorln("spinlock stuck: lock=%p value=0x%08x",
                      l, __atomic_load_n(l, __ATOMIC_RELAXED));
                hcf();
            }
        }
    }
}

void spinlock_unlock(spinlock_t* l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

extern "C" void *__memcpy(void * d, const void * s, uint64_t n) { 
    _memcpy(s, d, n);
    return d;
}

// 设置某一位为 1
void bitmap_set(u8* bitmap, u64 bit) {
    // bit >> 3 等价于 bit / 8
    // bit & 7 等价于 bit % 8
    bitmap[bit >> 3] |= (1U << (bit & 7));
}

// 清除某一位（设为 0）
void bitmap_clear(u8* bitmap, u64 bit) {
    bitmap[bit >> 3] &= ~(1U << (bit & 7));
}

// 获取某一位的值
bool bitmap_get(u8* bitmap, u64 bit) {
    return (bitmap[bit >> 3] & (1U << (bit & 7))) != 0;
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
