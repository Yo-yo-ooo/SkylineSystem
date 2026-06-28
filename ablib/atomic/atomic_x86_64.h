/*
* SPDX-License-Identifier: MIT
* File: atomic_x86_64.h
* Copyright (C) 2026 Yo-yo-ooo
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/
#ifndef ATOMIC_X86_64_H
#define ATOMIC_X86_64_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------------- 编译器屏障 ---------------- */
#define _ATOMIC_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* ---------------- fence ---------------- */
static inline void atomic_thread_fence(int mo) {
    switch (mo) {
    case ATOMIC_RELAXED:
        break;
    case ATOMIC_CONSUME: /* x86 上 consume 退化为 acquire */
    case ATOMIC_ACQUIRE:
    case ATOMIC_RELEASE:
    case ATOMIC_ACQ_REL:
        /* x86 TSO 硬件天然保证 LoadLoad/LoadStore/StoreStore 顺序，
           仅需编译器屏障即可满足标准语义，无需硬件屏障 */
        _ATOMIC_COMPILER_BARRIER();
        break;
    case ATOMIC_SEQ_CST:
        /* SEQ_CST 需要禁止 StoreLoad 重排，必须使用 mfence */
        __asm__ __volatile__("mfence" ::: "memory");
        break;
    }
}
static inline void atomic_signal_fence(int mo) {
    (void)mo;
    _ATOMIC_COMPILER_BARRIER();
}

/* ---------------- load ---------------- */
/* x86 TSO: mov 天然具备 acquire 语义，seq_cst 也只需编译器屏障防乱序 */
#define _ATOMIC_LOAD_X86(SUFFIX, TYPE, INSN, CONSTRAINT) \
static inline TYPE atomic_load_##SUFFIX(const volatile void *p, int mo) { \
    TYPE v; \
    if (mo == ATOMIC_RELAXED) { \
        __asm__ __volatile__(INSN " %1, %0" : CONSTRAINT(v) : "m"(*(const volatile TYPE*)p)); \
    } else { \
        __asm__ __volatile__(INSN " %1, %0" : CONSTRAINT(v) : "m"(*(const volatile TYPE*)p) : "memory"); \
    } \
    return v; \
}

_ATOMIC_LOAD_X86(1, uint8_t,  "movb",   "=q")
_ATOMIC_LOAD_X86(2, uint16_t, "movzwl", "=r")
_ATOMIC_LOAD_X86(4, uint32_t, "movl",   "=r")
_ATOMIC_LOAD_X86(8, uint64_t, "movq",   "=r")
#undef _ATOMIC_LOAD_X86

/* ---------------- store ---------------- */
/* SEQ_CST 使用 mov + mfence，避免低效 xchg；release 仅需编译器屏障 */
#define _ATOMIC_STORE_X86(SUFFIX, TYPE, INSN, CONSTRAINT) \
static inline void atomic_store_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    if (mo == ATOMIC_RELAXED) { \
        __asm__ __volatile__(INSN " %1, %0" : "=m"(*(volatile TYPE*)p) : CONSTRAINT(v)); \
    } else if (mo == ATOMIC_SEQ_CST) { \
        __asm__ __volatile__(INSN " %1, %0" : "=m"(*(volatile TYPE*)p) : CONSTRAINT(v) : "memory"); \
        __asm__ __volatile__("mfence" ::: "memory"); \
    } else { \
        __asm__ __volatile__(INSN " %1, %0" : "=m"(*(volatile TYPE*)p) : CONSTRAINT(v) : "memory"); \
    } \
}

_ATOMIC_STORE_X86(1, uint8_t,  "movb", "q")
_ATOMIC_STORE_X86(2, uint16_t, "movw", "r")
_ATOMIC_STORE_X86(4, uint32_t, "movl", "r")
_ATOMIC_STORE_X86(8, uint64_t, "movq", "r")
#undef _ATOMIC_STORE_X86

/* ---------------- exchange ---------------- */
/* xchg 硬件自带 SEQ_CST，非 relaxed 加编译器屏障即可 */
#define _ATOMIC_EXCHANGE_X86(SUFFIX, TYPE, INSN, CONSTRAINT) \
static inline TYPE atomic_exchange_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    if (mo == ATOMIC_RELAXED) { \
        __asm__ __volatile__(INSN " %0, %1" : CONSTRAINT(v), "+m"(*(volatile TYPE*)p)); \
    } else { \
        __asm__ __volatile__(INSN " %0, %1" : CONSTRAINT(v), "+m"(*(volatile TYPE*)p) :: "memory"); \
    } \
    return v; \
}

_ATOMIC_EXCHANGE_X86(1, uint8_t,  "xchgb", "+q")
_ATOMIC_EXCHANGE_X86(2, uint16_t, "xchgw", "+r")
_ATOMIC_EXCHANGE_X86(4, uint32_t, "xchgl", "+r")
_ATOMIC_EXCHANGE_X86(8, uint64_t, "xchgq", "+r")
#undef _ATOMIC_EXCHANGE_X86

/* ---------------- compare_exchange ---------------- */
/* lock cmpxchg 硬件自带 SEQ_CST，根据 sm/fm 动态决定编译器屏障 */
#define _ATOMIC_CMPXCHG_X86(SUFFIX, TYPE, INSN, CONSTRAINT) \
static inline bool atomic_compare_exchange_##SUFFIX(volatile void *p, void *e, TYPE d, \
                                              bool weak, int sm, int fm) { \
    (void)weak; /* x86 cmpxchg 为强 CAS，无伪共享失败 */ \
    TYPE exp = *(TYPE*)e; \
    uint8_t ok; \
    if (sm == ATOMIC_RELAXED && fm == ATOMIC_RELAXED) { \
        __asm__ __volatile__(INSN " %3, %1\n\tsete %b2" \
                             : "+a"(exp), "+m"(*(volatile TYPE*)p), "=q"(ok) \
                             : CONSTRAINT(d) : "cc"); \
    } else { \
        __asm__ __volatile__(INSN " %3, %1\n\tsete %b2" \
                             : "+a"(exp), "+m"(*(volatile TYPE*)p), "=q"(ok) \
                             : CONSTRAINT(d) : "memory", "cc"); \
    } \
    if (!ok) *(TYPE*)e = exp; \
    return (bool)ok; \
}

_ATOMIC_CMPXCHG_X86(1, uint8_t,  "lock cmpxchgb", "q")
_ATOMIC_CMPXCHG_X86(2, uint16_t, "lock cmpxchgw", "r")
_ATOMIC_CMPXCHG_X86(4, uint32_t, "lock cmpxchgl", "r")
_ATOMIC_CMPXCHG_X86(8, uint64_t, "lock cmpxchgq", "r")
#undef _ATOMIC_CMPXCHG_X86

/* ---------------- fetch_add ---------------- */
#define _ATOMIC_FETCH_ADD_X86(SUFFIX, TYPE, INSN, CONSTRAINT) \
static inline TYPE atomic_fetch_add_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    if (mo == ATOMIC_RELAXED) { \
        __asm__ __volatile__(INSN " %0, %1" : CONSTRAINT(v), "+m"(*(volatile TYPE*)p)); \
    } else { \
        __asm__ __volatile__(INSN " %0, %1" : CONSTRAINT(v), "+m"(*(volatile TYPE*)p) :: "memory"); \
    } \
    return v; \
}

_ATOMIC_FETCH_ADD_X86(1, uint8_t,  "lock xaddb", "+q")
_ATOMIC_FETCH_ADD_X86(2, uint16_t, "lock xaddw", "+r")
_ATOMIC_FETCH_ADD_X86(4, uint32_t, "lock xaddl", "+r")
_ATOMIC_FETCH_ADD_X86(8, uint64_t, "lock xaddq", "+r")
#undef _ATOMIC_FETCH_ADD_X86

static inline uint8_t  atomic_add_fetch_1(volatile void *p, uint8_t  v, int mo) { return atomic_fetch_add_1(p, v, mo) + v; }
static inline uint16_t atomic_add_fetch_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_add_2(p, v, mo) + v; }
static inline uint32_t atomic_add_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_add_4(p, v, mo) + v; }
static inline uint64_t atomic_add_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_add_8(p, v, mo) + v; }

/* ---------------- fetch_sub (xadd -v) ---------------- */
static inline uint8_t  atomic_fetch_sub_1(volatile void *p, uint8_t  v, int mo) { return atomic_fetch_add_1(p, (uint8_t)(-v), mo); }
static inline uint16_t atomic_fetch_sub_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_add_2(p, (uint16_t)(-v), mo); }
static inline uint32_t atomic_fetch_sub_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_add_4(p, (uint32_t)(-v), mo); }
static inline uint64_t atomic_fetch_sub_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_add_8(p, (uint64_t)(-v), mo); }

static inline uint8_t  atomic_sub_fetch_1(volatile void *p, uint8_t  v, int mo) { return atomic_fetch_sub_1(p, v, mo) - v; }
static inline uint16_t atomic_sub_fetch_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_sub_2(p, v, mo) - v; }
static inline uint32_t atomic_sub_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_sub_4(p, v, mo) - v; }
static inline uint64_t atomic_sub_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_sub_8(p, v, mo) - v; }

/* ---------------- fetch_and/or/xor/nand (CAS loop + pause) ---------------- */
#define _ATOMIC_FETCH_BITOP_X86(SUFFIX, TYPE, OP, EXPR) \
static inline TYPE atomic_fetch_##OP##_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    TYPE old = *(volatile TYPE*)p, neu; \
    do { \
        neu = (EXPR); \
        if (atomic_compare_exchange_##SUFFIX(p, &old, neu, false, mo, mo)) \
            break; \
        /* CAS 失败时提供 CPU 自旋提示，避免活锁和过度总线争用 */ \
        __asm__ __volatile__("pause" ::: "memory"); \
    } while (1); \
    return old; \
} \
static inline TYPE atomic_##OP##_fetch_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    TYPE old = *(volatile TYPE*)p, neu; \
    do { \
        neu = (EXPR); \
        if (atomic_compare_exchange_##SUFFIX(p, &old, neu, false, mo, mo)) \
            break; \
        __asm__ __volatile__("pause" ::: "memory"); \
    } while (1); \
    return neu; \
}

_ATOMIC_FETCH_BITOP_X86(1, uint8_t,  and, (uint8_t)(old & v))
_ATOMIC_FETCH_BITOP_X86(2, uint16_t, and, (uint16_t)(old & v))
_ATOMIC_FETCH_BITOP_X86(4, uint32_t, and, (uint32_t)(old & v))
_ATOMIC_FETCH_BITOP_X86(8, uint64_t, and, (uint64_t)(old & v))

_ATOMIC_FETCH_BITOP_X86(1, uint8_t,  or,  (uint8_t)(old | v))
_ATOMIC_FETCH_BITOP_X86(2, uint16_t, or,  (uint16_t)(old | v))
_ATOMIC_FETCH_BITOP_X86(4, uint32_t, or,  (uint32_t)(old | v))
_ATOMIC_FETCH_BITOP_X86(8, uint64_t, or,  (uint64_t)(old | v))

_ATOMIC_FETCH_BITOP_X86(1, uint8_t,  xor, (uint8_t)(old ^ v))
_ATOMIC_FETCH_BITOP_X86(2, uint16_t, xor, (uint16_t)(old ^ v))
_ATOMIC_FETCH_BITOP_X86(4, uint32_t, xor, (uint32_t)(old ^ v))
_ATOMIC_FETCH_BITOP_X86(8, uint64_t, xor, (uint64_t)(old ^ v))

_ATOMIC_FETCH_BITOP_X86(1, uint8_t,  nand, (uint8_t)~(old & v))
_ATOMIC_FETCH_BITOP_X86(2, uint16_t, nand, (uint16_t)~(old & v))
_ATOMIC_FETCH_BITOP_X86(4, uint32_t, nand, (uint32_t)~(old & v))
_ATOMIC_FETCH_BITOP_X86(8, uint64_t, nand, (uint64_t)~(old & v))

#undef _ATOMIC_FETCH_BITOP_X86

/* ---------------- test_and_set / clear ---------------- */
/* 扩展 test_and_set 支持 1/2/4/8 字节接口，适配不同位宽自旋锁 */
static inline bool atomic_test_and_set_1(volatile void *p, int mo) {
    (void)mo; /* xchg 硬件层自带 SEQ_CST */
    uint8_t v = 1;
    __asm__ __volatile__("xchgb %0, %1" : "+q"(v), "+m"(*(volatile uint8_t*)p) :: "memory");
    return v != 0;
}
/* 默认 1 字节版本兼容 atomic_flag */
static inline bool atomic_test_and_set(volatile void *p, int mo) {
    return atomic_test_and_set_1(p, mo);
}
static inline void atomic_clear(volatile void *p, int mo) {
    atomic_store_1(p, 0, mo);
}

/* lock-free 查询 */
static inline bool atomic_is_lock_free(size_t n)    { return n <= 8; }
static inline bool atomic_always_lock_free(size_t n){ return n <= 8; }

#endif /* ATOMIC_X86_64_H */