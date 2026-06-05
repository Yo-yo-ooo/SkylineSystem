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

static inline __attribute__((always_inline)) uint64_t __a_or_64(volatile uint64_t *p, uint64_t v)
{
    // 初始化旧值
    uint64_t old_val = __atomic_load_n(p, __ATOMIC_RELAXED);
    
    while (1) {
        uint64_t new_val = old_val | v;
        
        // 提前退出：如果位已存在，无需写操作
        if (new_val == old_val) {
            return old_val;
        }
        
        // 使用 CAS 尝试写入
        // success 使用 ACQ_REL 确保同步
        // failure 使用 RELAXED 即可，因为 old_val 会被自动更新
        if (__atomic_compare_exchange_n(p, &old_val, new_val, 
                                        1, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return old_val;
        }
        // CAS 失败时，old_val 已自动更新为当前内存值，无需额外加载
    }
}


static inline __attribute__((always_inline)) uint64_t __a_and_64(volatile uint64_t *p, uint64_t v)
{
    uint64_t old;
    __asm__ __volatile__(
        "movq %1, %0\n\t"
        "lock andq %2, %1\n\t"
        : "=r"(old), "+m"(*p)
        : "r"(v)
        : "memory", "cc"
    );
    return old;
}

static inline __attribute__((always_inline)) uint64_t __a_clear_bit(volatile uint64_t *p, uint64_t bit_mask)
{
    uint64_t old_val;
    
    __asm__ __volatile__(
        "1: movq %1, %0\n\t"
        "movq %0, %%r8\n\t"
        "andq %2, %%r8\n\t"
        "lock cmpxchgq %%r8, %1\n\t"
        "jne 1b\n\t"
        : "=&a"(old_val), "+m"(*p)
        : "r"(~bit_mask)
        : "r8", "memory", "cc"
    );
    
    return old_val;
}

#endif