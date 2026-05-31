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
static inline __attribute__((always_inline)) void	*__a_cas_p(volatile void *p, void *t, void *s)
{
	__asm__("lock ; cmpxchg %3, %1"
		: "=a"(t), "=m"(*(void *volatile *)p)
		: "a"(t), "r"(s) : "memory" );
	return (t);
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

static inline uint64_t __a_fetch_addu64(volatile uint64_t *p, uint64_t val)
{
    __asm__ volatile (
            "lock ; xaddq %0, %1"
            : "=r"(val), "=m"(*p)
            :"0"(val) : "memory" );
    return (val); // 返回加法执行前的值
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

static inline uint64_t __a_and_64(volatile uint64_t *p, uint64_t v)
{
    uint64_t old, new_val;
    
    // 获取当前值
    old = *p;
    
    do {
        new_val = old & v;
        
        /* * 使用 cmpxchgq：
         * 如果 *p == old，则将 new_val 写入 *p，并将旧值更新到 old；
         * 如果 *p != old，则将 *p 的当前值加载到 old。
         * "a" 约束强制使用 RAX 寄存器，这是 cmpxchg 指令的要求。
         */
        __asm__ volatile (
            "lock ; cmpxchgq %3, %1"
            : "=a"(old), "+m"(*p)
            : "a"(old), "r"(new_val)
            : "memory", "cc"
        );
    } while (old != (old & v)); // 当 cmpxchg 成功时，循环结束
    
    return old; // 返回操作前的旧值
}


#endif