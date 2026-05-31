#pragma once
#ifndef _x86_64_ATOMIC_COMMON_H_
#define _x86_64_ATOMIC_COMMON_H_

#include <stdint.h>
#include <stddef.h>


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
/* * 2. FETCH_SUB
 * x86 没有专用的 xsub，但加上一个负数等价于减法。
 * 利用二进制补码特性，将 val 取负后使用 xaddq 即可。
 */
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

static inline __attribute__((always_inline))
uint32_t __a_subu32(volatile uint32_t *p, uint32_t val)
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

static inline __attribute__((always_inline)) void	__a_or_64(volatile uint64_t *p, uint64_t v)
{
	__asm__	volatile (
			"lock ; or %1, %0"
			: "=m"(*p) : "r"(v) : "memory" );
}

static inline __attribute__((always_inline)) uint64_t __a_and_64(volatile uint64_t *p, uint64_t v)
{
    uint64_t old;
    
    __asm__ __volatile__(
        "1:\n\t"
        "movq %1, %0\n\t"          // 加载当前值到old
        "movq %0, %%r8\n\t"        // 保存原始值到r8
        "andq %2, %0\n\t"          // 计算新值
        "lock cmpxchgq %0, %1\n\t" // 尝试CAS
        "jne 1b\n\t"               // 如果失败，重试
        : "=&a"(old), "+m"(*p)
        : "r"(v)
        : "r8", "memory", "cc"
    );
    
    return old; // 返回操作前的原始值
}

#endif