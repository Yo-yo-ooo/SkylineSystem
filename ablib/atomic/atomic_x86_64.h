/*
* SPDX-License-Identifier: MIT
* File: atomic_x86_64.h
* Copyright (C) 2026 Yo-yoo-ooo
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

/* ========================================================================= *
 * 严格使用约束：                                                           *
 * 原子变量必须自然对齐至自身类型宽度 (如 uint64_t 必须 8 字节对齐)。       *
 * 未对齐的内存访问在 x86 上不保证硬件原子性，会产生撕裂读/写。             *
 * ========================================================================= */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifndef ATOMIC_RELAXED
#define ATOMIC_RELAXED  0
#define ATOMIC_CONSUME  1
#define ATOMIC_ACQUIRE  2
#define ATOMIC_RELEASE  3
#define ATOMIC_ACQ_REL  4
#define ATOMIC_SEQ_CST  5
#endif

#if defined(__GNUC__) || defined(__clang__)
#define ATOMIC_EXT __extension__
#define ATOMIC_INLINE static inline __attribute__((always_inline))
#else
#define ATOMIC_EXT
#define ATOMIC_INLINE static inline
#endif

#define X86_ATOMIC_MAX_LOCKFREE 8

/* ---------------- 编译器屏障与判定宏 ---------------- */
#define _ATOMIC_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

#define _ATOMIC_PRE_BAR(mo)  ((mo) == ATOMIC_RELEASE || (mo) == ATOMIC_ACQ_REL || (mo) == ATOMIC_SEQ_CST)
#define _ATOMIC_POST_BAR(mo) ((mo) == ATOMIC_ACQUIRE || (mo) == ATOMIC_CONSUME || (mo) == ATOMIC_ACQ_REL || (mo) == ATOMIC_SEQ_CST)

/* ---------------- CAS 内存序合法性校验 ---------------- */
#if defined(__GNUC__) || defined(__clang__)
extern void _atomic_invalid_cas_mo(void) __attribute__((error("CAS fm cannot be release/acq_rel and fm <= sm")));
#define _ATOMIC_CHECK_CAS_MO(sm, fm) do { \
    if ((fm) == ATOMIC_RELEASE || (fm) == ATOMIC_ACQ_REL || (fm) > (sm)) { \
        if (__builtin_constant_p(sm) && __builtin_constant_p(fm)) \
            _atomic_invalid_cas_mo(); \
    } \
} while (0)
#else
#define _ATOMIC_CHECK_CAS_MO(sm, fm) ((void)0)
#endif

/* ---------------- fence ---------------- */
ATOMIC_EXT ATOMIC_INLINE void atomic_thread_fence(int mo) {
    switch (mo) {
    case ATOMIC_RELAXED:
        break;
    case ATOMIC_CONSUME:
    case ATOMIC_ACQUIRE:
    case ATOMIC_RELEASE:
    case ATOMIC_ACQ_REL:
        _ATOMIC_COMPILER_BARRIER();
        break;
    case ATOMIC_SEQ_CST:
        __asm__ __volatile__("mfence" ::: "memory");
        break;
    }
}

ATOMIC_EXT ATOMIC_INLINE void atomic_signal_fence(int mo) {
    (void)mo;
    _ATOMIC_COMPILER_BARRIER();
}

/* ---------------- load ---------------- */
#define _ATOMIC_LOAD_X86(SUFFIX, TYPE, INSN, CONSTRAINT) \
ATOMIC_EXT ATOMIC_INLINE TYPE atomic_load_##SUFFIX(const volatile void *p, int mo) { \
    TYPE v; \
    __asm__ __volatile__(INSN " %1, %0" : CONSTRAINT(v) : "m"(*(const volatile TYPE*)p)); \
    if (_ATOMIC_POST_BAR(mo)) _ATOMIC_COMPILER_BARRIER(); \
    return v; \
}

_ATOMIC_LOAD_X86(1, uint8_t,  "movb", "=q")
_ATOMIC_LOAD_X86(2, uint16_t, "movw", "=r")
_ATOMIC_LOAD_X86(4, uint32_t, "movl", "=r")
_ATOMIC_LOAD_X86(8, uint64_t, "movq", "=r")
#undef _ATOMIC_LOAD_X86

/* ---------------- store ---------------- */
#define _ATOMIC_STORE_X86(SUFFIX, TYPE, INSN, CONSTRAINT) \
ATOMIC_EXT ATOMIC_INLINE void atomic_store_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    if (_ATOMIC_PRE_BAR(mo)) _ATOMIC_COMPILER_BARRIER(); \
    __asm__ __volatile__(INSN " %1, %0" : "=m"(*(volatile TYPE*)p) : CONSTRAINT(v)); \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("mfence" ::: "memory"); \
}

_ATOMIC_STORE_X86(1, uint8_t,  "movb", "q")
_ATOMIC_STORE_X86(2, uint16_t, "movw", "r")
_ATOMIC_STORE_X86(4, uint32_t, "movl", "r")
_ATOMIC_STORE_X86(8, uint64_t, "movq", "r")
#undef _ATOMIC_STORE_X86

/* ---------------- exchange ---------------- */
#define _ATOMIC_EXCHANGE_X86(SUFFIX, TYPE, INSN, CONSTRAINT) \
ATOMIC_EXT ATOMIC_INLINE TYPE atomic_exchange_##SUFFIX(volatile void *p, TYPE v, int mo) { \
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
#define _ATOMIC_CMPXCHG_X86(SUFFIX, TYPE, INSN, CONSTRAINT) \
ATOMIC_EXT ATOMIC_INLINE bool atomic_compare_exchange_##SUFFIX(volatile void *p, void *e, TYPE d, \
                                              bool weak, int sm, int fm) { \
    (void)weak; \
    _ATOMIC_CHECK_CAS_MO(sm, fm); \
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
ATOMIC_EXT ATOMIC_INLINE TYPE atomic_fetch_add_##SUFFIX(volatile void *p, TYPE v, int mo) { \
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

ATOMIC_EXT ATOMIC_INLINE uint8_t  atomic_add_fetch_1(volatile void *p, uint8_t  v, int mo) { return atomic_fetch_add_1(p, v, mo) + v; }
ATOMIC_EXT ATOMIC_INLINE uint16_t atomic_add_fetch_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_add_2(p, v, mo) + v; }
ATOMIC_EXT ATOMIC_INLINE uint32_t atomic_add_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_add_4(p, v, mo) + v; }
ATOMIC_EXT ATOMIC_INLINE uint64_t atomic_add_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_add_8(p, v, mo) + v; }

/* ---------------- fetch_sub (xadd 0-v) ---------------- */
ATOMIC_EXT ATOMIC_INLINE uint8_t  atomic_fetch_sub_1(volatile void *p, uint8_t  v, int mo) { return atomic_fetch_add_1(p, (uint8_t)(0 - v),  mo); }
ATOMIC_EXT ATOMIC_INLINE uint16_t atomic_fetch_sub_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_add_2(p, (uint16_t)(0 - v), mo); }
ATOMIC_EXT ATOMIC_INLINE uint32_t atomic_fetch_sub_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_add_4(p, (uint32_t)(0 - v), mo); }
ATOMIC_EXT ATOMIC_INLINE uint64_t atomic_fetch_sub_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_add_8(p, (uint64_t)(0 - v), mo); }

ATOMIC_EXT ATOMIC_INLINE uint8_t  atomic_sub_fetch_1(volatile void *p, uint8_t  v, int mo) { return atomic_fetch_sub_1(p, v, mo) - v; }
ATOMIC_EXT ATOMIC_INLINE uint16_t atomic_sub_fetch_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_sub_2(p, v, mo) - v; }
ATOMIC_EXT ATOMIC_INLINE uint32_t atomic_sub_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_sub_4(p, v, mo) - v; }
ATOMIC_EXT ATOMIC_INLINE uint64_t atomic_sub_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_sub_8(p, v, mo) - v; }

/* ---------------- fetch_and/or/xor/nand (CAS loop + pause) ---------------- */

/* 自动计算合法的 CAS failure 内存序。
   C11 规定 fm 不可为 release/acq_rel。因此当 sm 为 acq_rel 时，fm 降级为 acquire；
   当 sm 为 release 时，fm 降级为 relaxed。 */
#define _ATOMIC_CAS_FAIL_MO(mo) \
    ((mo) == ATOMIC_SEQ_CST ? ATOMIC_SEQ_CST : \
     (mo) == ATOMIC_ACQ_REL ? ATOMIC_ACQUIRE : \
     (mo) == ATOMIC_RELEASE ? ATOMIC_RELAXED : \
     (mo))

#define _ATOMIC_FETCH_BITOP_X86(SUFFIX, TYPE, OP, EXPR) \
ATOMIC_EXT ATOMIC_INLINE TYPE atomic_fetch_##OP##_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    TYPE old = atomic_load_##SUFFIX(p, mo), neu; \
    int fail_mo = _ATOMIC_CAS_FAIL_MO(mo); \
    do { \
        neu = (EXPR); \
        if (atomic_compare_exchange_##SUFFIX(p, &old, neu, false, mo, fail_mo)) \
            break; \
        __asm__ __volatile__("pause" ::: "memory"); \
    } while (1); \
    return old; \
} \
ATOMIC_EXT ATOMIC_INLINE TYPE atomic_##OP##_fetch_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    TYPE old = atomic_fetch_##OP##_##SUFFIX(p, v, mo); \
    return (EXPR); \
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
#undef _ATOMIC_CAS_FAIL_MO

/* ---------------- test_and_set / clear ---------------- */
ATOMIC_EXT ATOMIC_INLINE bool atomic_test_and_set(volatile void *p, int mo) {
    uint8_t v = 1;
    if (mo == ATOMIC_RELAXED) {
        __asm__ __volatile__("xchgb %0, %1" : "+q"(v), "+m"(*(volatile uint8_t*)p));
    } else {
        __asm__ __volatile__("xchgb %0, %1" : "+q"(v), "+m"(*(volatile uint8_t*)p) :: "memory");
    }
    return v != 0;
}

ATOMIC_EXT ATOMIC_INLINE void atomic_clear(volatile void *p, int mo) {
    atomic_store_1(p, 0, mo);
}

/* lock-free 查询 */
ATOMIC_EXT ATOMIC_INLINE bool atomic_is_lock_free(size_t n)    { return n <= X86_ATOMIC_MAX_LOCKFREE; }
ATOMIC_EXT ATOMIC_INLINE bool atomic_always_lock_free(size_t n){ return n <= X86_ATOMIC_MAX_LOCKFREE; }

#endif /* ATOMIC_X86_64_H */