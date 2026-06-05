#pragma once
#ifndef _x86_64_ATOMIC_COMMON_H_
#define _x86_64_ATOMIC_COMMON_H_

#include <stdint.h>
#include <stddef.h>

// [修复] 引入 CPU_RELAX 宏，利用 pause 指令防止无锁重试循环导致的系统活锁与总线风暴
#define CPU_RELAX() __asm__ __volatile__("pause" ::: "memory")

#define A_CAS_U64_ASM(p, old, new) ({ \
    uint64_t __old = (old); \
    uint64_t __new = (new); \
    __asm__ __volatile__("lock cmpxchgq %2, %1" \
        : "+a"(__old), "+m"(*(volatile uint64_t *)(p)) \
        : "r"(__new) \
        : "memory", "cc"); \
    __old; \
})

static inline __attribute__((always_inline)) void *__a_cas_p(volatile void *p, void *t, void *s)
{
    __asm__ __volatile__(
        "lock cmpxchg %3, %1"
        : "+a"(t), "+m"(*(void *volatile *)p)
        : "r"(s)
        : "memory", "cc"
    );
    return t;
}

static inline __attribute__((always_inline)) uint32_t __a_fetch_addu32(volatile uint32_t *p, uint32_t v)
{
    __asm__ volatile (
        "lock ; xaddl %0, %1"
        : "+r"(v), "+m"(*p)
        :
        : "memory", "cc"
    );
    return v;
}

static inline __attribute__((always_inline)) uint64_t __a_fetch_addu64(volatile uint64_t *p, uint64_t val)
{
    __asm__ volatile (
        "lock xaddq %0, %1"
        : "+r"(val), "+m"(*p)
        :
        : "memory", "cc"
    );
    return val;
}

#define A_FETCH_SUB_U64(p, val) ({ \
    uint64_t __res = -(uint64_t)(val); \
    __asm__ __volatile__( \
        "lock ; xaddq %0, %1" \
        : "+r" (__res), "+m" (*(volatile uint64_t *)(p)) \
        : \
        : "memory", "cc"); \
    __res; \
})

static inline __attribute__((always_inline)) uint64_t __a_subu64(volatile uint64_t *p, uint64_t val)
{
    uint64_t __res = -val;
    __asm__ __volatile__(
        "lock ; xaddq %0, %1"
        : "+r" (__res), "+m" (*p)
        :
        : "memory", "cc"
    );
    return __res;
}

static inline __attribute__((always_inline)) uint32_t __a_subu32(volatile uint32_t *p, uint32_t val)
{
    uint32_t res = (uint32_t)(-(int32_t)val);
    __asm__ volatile (
        "lock; xaddl %0, %1"
        : "+r" (res), "+m" (*p)
        :
        : "memory", "cc"
    );
    return res;
}

static inline __attribute__((always_inline)) void __a_or_64(volatile uint64_t *p, uint64_t v)
{
    // [修复] 将 "=m" (只写) 修改为 "+m" (读写)，并显式指定 orq 指令和 cc 状态改变
    __asm__ volatile (
            "lock ; orq %1, %0"
            : "+m"(*p) : "r"(v) : "memory", "cc" );
}

// atomic_common.h中替换__a_and_64实现
static inline __attribute__((always_inline)) uint64_t __a_and_64(volatile uint64_t *p, uint64_t v)
{
    uint64_t old;
    __asm__ __volatile__(
        "1: movq %1, %0\n\t"
        "movq %0, %%r8\n\t"
        "andq %2, %%r8\n\t"
        "lock cmpxchgq %%r8, %1\n\t"
        "jne 1b\n\t"
        : "=&a"(old), "+m"(*p)
        : "r"(v)
        : "r8", "memory", "cc"
    );
    return old;
}

#endif