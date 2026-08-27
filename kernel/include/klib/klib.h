//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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
#include <klib/fifo.h>
#include <klib/serial.h>
#include <klib/ctype.h>

extern volatile uint64_t hhdm_offset;
extern volatile uint64_t RSDP_ADDR;

#define PAGE_SIZE 4096

#define DIV_ROUND_UP(x, y) (((x) + ((y) - 1)) / (y))
#define ALIGN_UP(x, y)     DIV_ROUND_UP(x, y) * y
#define ALIGN_DOWN(x, y)   ((x / y) * y)

#define HIGHER_HALF(x)   (decltype(x))((uint64_t)x + hhdm_offset)
#define VIRTUAL(ptr)       HIGHER_HALF(ptr)
#define PHYSICAL(x)      (decltype(x))((uint64_t)x - hhdm_offset)
#define FORCE_INLINE inline __attribute__((always_inline))

extern "C" void Panic(const char* message);
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
/*template <typename T>
T CFCast(auto F){
	union FT
	{T   t;decltype(F) f;};
	FT ft;ft.f=F;
	return ft.t;  //此为技巧，利用联合将地址返回。
}*/

#define __init
#ifdef __x86_64__
#define __ffunc __attribute__((target("sse2")))
#define _intr __attribute__((interrupt))
extern uint32_t MaxXsaveSize;
#else
#define __ffunc
#endif
#define UnCompleteCode 1


uint16_t kld_16 (const uint8_t* ptr);
uint32_t kld_32 (const uint8_t* ptr);
uint64_t kld_64 (const uint8_t* ptr);

void qsort(void *base, size_t num, size_t width, int32_t (*sort)(const void *e1, const void *e2));


namespace Interrupt {
    // 返回 1 表示中断开启，0 表示中断关闭
    static inline int32_t State() {
    #ifdef __x86_64__
        uint64_t rflags;
        __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(rflags) :: "memory");
        return (rflags >> 9) & 1;
    #elif defined(__aarch64__)
        uint64_t daif;
        __asm__ volatile ("mrs %0, daif" : "=r"(daif));
        return !(daif & (1u << 7));
    #elif defined(__riscv)
        uint64_t sstatus;
        __asm__ volatile ("csrr %0, sstatus" : "=r"(sstatus));
        return (sstatus >> 1) & 1;
    #else
    #error "Interrupt: unsupported architecture"
    #endif
    }

    static inline void Mask() {
    #ifdef __x86_64__
        __asm__ volatile ("cli" ::: "memory");
    #elif defined(__aarch64__)
        // DAIFSet imm 是位选择掩码: bit0=F, bit1=I, bit2=A, bit3=D
        __asm__ volatile ("msr daifset, #2" ::: "memory");
    #elif defined(__riscv)
        __asm__ volatile ("csrci sstatus, 2" ::: "memory");
    #endif
    }

    static inline void Unmask() {
    #ifdef __x86_64__
        __asm__ volatile ("sti" ::: "memory");
    #elif defined(__aarch64__)
        __asm__ volatile ("msr daifclr, #2" ::: "memory");
    #elif defined(__riscv)
        __asm__ volatile ("csrsi sstatus, 2" ::: "memory");
    #endif
    }

    // --- 嵌套安全的 save/restore ---
    // 保存当前状态并屏蔽中断；返回 true = 进入前中断是开的。
    static inline bool SaveMask() {
        bool was = State();
        Mask();
        return was;
    }
    // 仅当进入前是开的才恢复（之前本来就关则保持关）。
    static inline void Restore(bool was) {
        if (was) Unmask();
    }
}

// 只关中断，不拿锁（保护 per-CPU 数据）
struct IrqSave {
    bool prev;
    IrqSave()  { prev = Interrupt::SaveMask(); }
    ~IrqSave() { Interrupt::Restore(prev); }
    IrqSave(const IrqSave&)            = delete;
    IrqSave& operator=(const IrqSave&) = delete;
};

// 关中断 + 拿全局锁（顺序: 先关中断再拿锁，
// 防止拿锁后被本核中断、中断处理程序又来抢同一把锁）
struct IrqSpinGuard {
    spinlock_t* l;
    bool prev;
    explicit IrqSpinGuard(spinlock_t* lock) : l(lock) {
        prev = Interrupt::SaveMask();
        spinlock_lock(l);
    }
    ~IrqSpinGuard() {
        spinlock_unlock(l);
        Interrupt::Restore(prev);
    }
    IrqSpinGuard(const IrqSpinGuard&)            = delete;
    IrqSpinGuard& operator=(const IrqSpinGuard&) = delete;
};


extern "C" {
//freestanding cpu features functions
func_optimize(3) void *memset_fscpuf(void *dst, const int32_t val, size_t n);
func_optimize(3) void *memcpy_fscpuf(void *dst, const void *src, size_t n);
func_optimize(3) void *memmove_fscpuf(void *dst, const void *src, size_t n);
func_optimize(3) int32_t memcmp_fscpuf(const void *left, const void *right, size_t len);
func_optimize(3) void bzero(void *dst, size_t n);
}

#endif