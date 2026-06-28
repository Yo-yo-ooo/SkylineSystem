/*
* SPDX-License-Identifier: MIT
* File: atomic_riscv64.h
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
#ifndef ATOMIC_RISCV64_H
#define ATOMIC_RISCV64_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define _ATOMIC_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* ---------------- fence ---------------- */
static inline void atomic_thread_fence(int mo) {
    switch (mo) {
    case ATOMIC_RELAXED: break;
    case ATOMIC_CONSUME:
    case ATOMIC_ACQUIRE: __asm__ __volatile__("fence r,rw" ::: "memory"); break;
    case ATOMIC_RELEASE: __asm__ __volatile__("fence rw,r" ::: "memory"); break;
    case ATOMIC_ACQ_REL:
    case ATOMIC_SEQ_CST: __asm__ __volatile__("fence rw,rw" ::: "memory"); break;
    }
}
static inline void atomic_signal_fence(int mo) { (void)mo; _ATOMIC_COMPILER_BARRIER(); }

/* 4/8 字节 AMO 后缀选择 */
#define _RV_AMO_SUFFIX(mo) \
    ((mo)==ATOMIC_RELAXED ? "" : \
     (mo)==ATOMIC_ACQUIRE ? ".aq" : \
     (mo)==ATOMIC_RELEASE ? ".rl" : ".aqrl")

/* SEQ_CST 需要 fence r,rw 前置（在 .aqrl 之上） */
#define _RV_PRE_SC(mo) \
    do { if ((mo)==ATOMIC_SEQ_CST) __asm__ __volatile__("fence r,rw" ::: "memory"); } while (0)

/* ---------------- load ---------------- */
/* RISC-V 对齐的子字访问是原子的；acquire/SC 用 fence */
#define _RV_LOAD(SUF, TYPE, INSN) \
static inline TYPE atomic_load_##SUF(const volatile void *p, int mo) { \
    TYPE v; \
    if (mo == ATOMIC_RELAXED) { \
        __asm__ __volatile__(INSN " %0, %1" : "=r"(v) : "Q"(*(const volatile TYPE*)p)); \
    } else { \
        __asm__ __volatile__(INSN " %0, %1" : "=r"(v) : "Q"(*(const volatile TYPE*)p) : "memory"); \
        if (mo == ATOMIC_ACQUIRE || mo == ATOMIC_CONSUME || mo == ATOMIC_SEQ_CST) \
            __asm__ __volatile__("fence r,rw" ::: "memory"); \
    } \
    return v; \
}

_RV_LOAD(1, uint8_t,  "lbu")
_RV_LOAD(2, uint16_t, "lhu")
_RV_LOAD(4, uint32_t, "lwu")
_RV_LOAD(8, uint64_t, "ld")
#undef _RV_LOAD

/* ---------------- store ---------------- */
/* release 用 fence rw,r 前置；SEQ_CST store 用 amoswap.rl */
#define _RV_STORE(SUF, TYPE, INSN, AMO, CAST) \
static inline void atomic_store_##SUF(volatile void *p, TYPE v, int mo) { \
    if (mo == ATOMIC_SEQ_CST) { \
        __asm__ __volatile__(AMO ".rl x0, %0, 0(%1)" :: "r"((CAST)v), "r"((uintptr_t)p) : "memory"); \
    } else { \
        if (mo == ATOMIC_RELEASE) __asm__ __volatile__("fence rw,r" ::: "memory"); \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__(INSN " %0, %1" : : "r"(v), "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__(INSN " %0, %1" : : "r"(v), "Q"(*(volatile TYPE*)p) : "memory"); \
    } \
}

_RV_STORE(1, uint8_t,  "sb", "amoswap.w", (uint32_t))
_RV_STORE(2, uint16_t, "sh", "amoswap.w", (uint32_t))
_RV_STORE(4, uint32_t, "sw", "amoswap.w", (uint32_t))
_RV_STORE(8, uint64_t, "sd", "amoswap.d", (uint64_t))
#undef _RV_STORE

/* ---------------- 4/8 字节 AMO 通用模板 (add/sub/and/or/xor) ---------------- */
#define _RV_AMO_RMW(SUF, W, TYPE, NAME, AMO, OP) \
static inline TYPE atomic_fetch_##NAME##_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old; _RV_PRE_SC(mo); \
    __asm__ __volatile__(AMO _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)" \
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory"); \
    return old; \
} \
static inline TYPE atomic_##NAME##_fetch_##SUF(volatile void *p, TYPE v, int mo) { \
    return atomic_fetch_##NAME##_##SUF(p, v, mo) OP v; \
}

_RV_AMO_RMW(4, w, uint32_t, add, "amoadd.w", +)
_RV_AMO_RMW(8, d, uint64_t, add, "amoadd.d", +)

_RV_AMO_RMW(4, w, uint32_t, sub, "amoadd.w", -) /* fetch_sub 用 add -v 实现 */
_RV_AMO_RMW(8, d, uint64_t, sub, "amoadd.d", -)

_RV_AMO_RMW(4, w, uint32_t, and, "amoand.w", &)
_RV_AMO_RMW(8, d, uint64_t, and, "amoand.d", &)

_RV_AMO_RMW(4, w, uint32_t, or,  "amoor.w",  |)
_RV_AMO_RMW(8, d, uint64_t, or,  "amoor.d",  |)

_RV_AMO_RMW(4, w, uint32_t, xor, "amoxor.w", ^)
_RV_AMO_RMW(8, d, uint64_t, xor, "amoxor.d", ^)
#undef _RV_AMO_RMW

/* fetch_sub 需要转换负数 */
static inline uint32_t atomic_fetch_sub_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_add_4(p, (uint32_t)(-v), mo); }
static inline uint64_t atomic_fetch_sub_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_add_8(p, (uint64_t)(-v), mo); }

/* ---------------- exchange (amoswap) ---------------- */
#define _RV_AMO_SWAP(SUF, W, TYPE) \
static inline TYPE atomic_exchange_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old; _RV_PRE_SC(mo); \
    __asm__ __volatile__("amoswap." #W _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)" \
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory"); \
    return old; \
}

_RV_AMO_SWAP(4, w, uint32_t)
_RV_AMO_SWAP(8, d, uint64_t)
#undef _RV_AMO_SWAP

/* ---------------- compare_exchange (4/8 字节 LR/SC) ---------------- */
#define _RV_CAS(SUF, W, TYPE) \
static inline bool atomic_compare_exchange_##SUF(volatile void *p, void *e, TYPE d, \
                                                 bool weak, int sm, int fm) { \
    (void)weak; (void)fm; \
    TYPE exp = *(TYPE*)e, cur; long ok; \
    _RV_PRE_SC(sm); \
    __asm__ __volatile__( \
        "1: lr." #W _RV_AMO_SUFFIX(sm) " %0, 0(%3)\n" \
        "   bne  %0, %2, 3f\n" \
        "   sc." #W _RV_AMO_SUFFIX(sm) " %1, %4, 0(%3)\n" \
        "   bnez %1, 1b\n" \
        "3:\n" \
        : "=&r"(cur), "=&r"(ok) \
        : "r"(exp), "r"((uintptr_t)p), "r"(d) \
        : "memory"); \
    if (cur != exp) { *(TYPE*)e = cur; return false; } \
    return true; \
}

_RV_CAS(4, w, uint32_t)
_RV_CAS(8, d, uint64_t)
#undef _RV_CAS

/* ---------------- 1/2 字节 CAS (基于 4 字节 LR/SC 掩码) ---------------- */
#define _RV_SUBWORD_CAS(SUF, TYPE, BITS) \
static inline bool atomic_compare_exchange_##SUF(volatile void *p, void *e, TYPE d, \
                                                 bool weak, int sm, int fm) { \
    (void)weak; (void)fm; \
    uintptr_t addr = (uintptr_t)p; \
    uintptr_t aligned = addr & ~3UL; \
    unsigned shift = (unsigned)((addr & 3UL) * 8); \
    uint32_t mask = ((1U << BITS) - 1) << shift; \
    TYPE exp = *(TYPE*)e; \
    uint32_t expw = (uint32_t)exp << shift; \
    uint32_t desw = (uint32_t)d << shift; \
    uint32_t oldw, neww; long ok; \
    _RV_PRE_SC(sm); \
    do { \
        __asm__ __volatile__("lr.w" _RV_AMO_SUFFIX(sm) " %0, 0(%1)" : "=r"(oldw) : "r"(aligned) : "memory"); \
        if ((oldw & mask) != expw) { \
            *(TYPE*)e = (TYPE)((oldw & mask) >> shift); \
            return false; \
        } \
        neww = (oldw & ~mask) | desw; \
        __asm__ __volatile__("sc.w" _RV_AMO_SUFFIX(sm) " %0, %1, 0(%2)" : "=&r"(ok) : "r"(neww), "r"(aligned) : "memory"); \
    } while (ok); \
    return true; \
}

_RV_SUBWORD_CAS(1, uint8_t,  8)
_RV_SUBWORD_CAS(2, uint16_t, 16)
#undef _RV_SUBWORD_CAS

/* ---------------- 1/2 字节 RMW (基于 4 字节 LR/SC 掩码运算) ---------------- */
#define _RV_SUBWORD_RMW(SUF, TYPE, BITS, NAME, EXPR) \
static inline TYPE atomic_fetch_##NAME##_##SUF(volatile void *p, TYPE v, int mo) { \
    uintptr_t addr = (uintptr_t)p; \
    uintptr_t aligned = addr & ~3UL; \
    unsigned shift = (unsigned)((addr & 3UL) * 8); \
    uint32_t mask = ((1U << BITS) - 1) << shift; \
    uint32_t oldw, neww, oldv; long ok; \
    _RV_PRE_SC(mo); \
    do { \
        __asm__ __volatile__("lr.w" _RV_AMO_SUFFIX(mo) " %0, 0(%1)" : "=r"(oldw) : "r"(aligned) : "memory"); \
        oldv = (oldw & mask) >> shift; \
        neww = (oldw & ~mask) | ((((uint32_t)(EXPR)) << shift) & mask); \
        __asm__ __volatile__("sc.w" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)" : "=&r"(ok) : "r"(neww), "r"(aligned) : "memory"); \
    } while (ok); \
    return (TYPE)oldv; \
} \
static inline TYPE atomic_##NAME##_fetch_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old = atomic_fetch_##NAME##_##SUF(p, v, mo); \
    return (TYPE)(EXPR); \
}

_RV_SUBWORD_RMW(1, uint8_t,  8,  add, oldv + v)
_RV_SUBWORD_RMW(2, uint16_t, 16, add, oldv + v)

_RV_SUBWORD_RMW(1, uint8_t,  8,  sub, oldv - v)
_RV_SUBWORD_RMW(2, uint16_t, 16, sub, oldv - v)

_RV_SUBWORD_RMW(1, uint8_t,  8,  and, oldv & v)
_RV_SUBWORD_RMW(2, uint16_t, 16, and, oldv & v)

_RV_SUBWORD_RMW(1, uint8_t,  8,  or,  oldv | v)
_RV_SUBWORD_RMW(2, uint16_t, 16, or,  oldv | v)

_RV_SUBWORD_RMW(1, uint8_t,  8,  xor, oldv ^ v)
_RV_SUBWORD_RMW(2, uint16_t, 16, xor, oldv ^ v)

_RV_SUBWORD_RMW(1, uint8_t,  8,  nand, ~(oldv & v))
_RV_SUBWORD_RMW(2, uint16_t, 16, nand, ~(oldv & v))
#undef _RV_SUBWORD_RMW

/* nand_fetch 的 EXPR 实际上是新值，但 fetch 返回旧值，所以需要修正 nand_fetch 的返回值 */
#define _RV_NAND_FETCH_FIX(SUF, TYPE) \
static inline TYPE atomic_nand_fetch_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old = atomic_fetch_nand_##SUF(p, v, mo); \
    return (TYPE)~(old & v); \
}
_RV_NAND_FETCH_FIX(1, uint8_t)
_RV_NAND_FETCH_FIX(2, uint16_t)
#undef _RV_NAND_FETCH_FIX

/* 1/2 字节 exchange (基于 CAS 循环) */
#define _RV_SUBWORD_EXCHANGE(SUF, TYPE) \
static inline TYPE atomic_exchange_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old = *(volatile TYPE*)p; \
    do { } while (!atomic_compare_exchange_##SUF(p, &old, v, false, mo, mo)); \
    return old; \
}
_RV_SUBWORD_EXCHANGE(1, uint8_t)
_RV_SUBWORD_EXCHANGE(2, uint16_t)
#undef _RV_SUBWORD_EXCHANGE

/* ---------------- test_and_set / clear ---------------- */
static inline bool atomic_test_and_set(volatile void *p, int mo) {
    return atomic_exchange_1(p, 1, mo) != 0;
}
static inline void atomic_clear(volatile void *p, int mo) {
    atomic_store_1(p, 0, mo);
}

static inline bool atomic_is_lock_free(size_t n)    { return n <= 8; }
static inline bool atomic_always_lock_free(size_t n){ return n <= 8; }

#endif /* ATOMIC_RISCV64_H */