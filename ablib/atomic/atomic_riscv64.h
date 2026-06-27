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
static inline uint8_t atomic_load_1(const volatile void *p, int mo) {
    uint8_t v;
    __asm__ __volatile__("lbu %0, %1" : "=r"(v) : "Q"(*(const volatile uint8_t*)p) : "memory");
    if (mo == ATOMIC_ACQUIRE || mo == ATOMIC_CONSUME) __asm__ __volatile__("fence r,rw" ::: "memory");
    else if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("fence r,rw" ::: "memory");
    return v;
}
static inline uint16_t atomic_load_2(const volatile void *p, int mo) {
    uint16_t v;
    __asm__ __volatile__("lhu %0, %1" : "=r"(v) : "Q"(*(const volatile uint16_t*)p) : "memory");
    if (mo == ATOMIC_ACQUIRE || mo == ATOMIC_CONSUME || mo == ATOMIC_SEQ_CST)
        __asm__ __volatile__("fence r,rw" ::: "memory");
    return v;
}
static inline uint32_t atomic_load_4(const volatile void *p, int mo) {
    uint32_t v;
    __asm__ __volatile__("lwu %0, %1" : "=r"(v) : "Q"(*(const volatile uint32_t*)p) : "memory");
    if (mo == ATOMIC_ACQUIRE || mo == ATOMIC_CONSUME || mo == ATOMIC_SEQ_CST)
        __asm__ __volatile__("fence r,rw" ::: "memory");
    return v;
}
static inline uint64_t atomic_load_8(const volatile void *p, int mo) {
    uint64_t v;
    __asm__ __volatile__("ld %0, %1" : "=r"(v) : "Q"(*(const volatile uint64_t*)p) : "memory");
    if (mo == ATOMIC_ACQUIRE || mo == ATOMIC_CONSUME || mo == ATOMIC_SEQ_CST)
        __asm__ __volatile__("fence r,rw" ::: "memory");
    return v;
}

/* ---------------- store ---------------- */
/* release 用 fence rw,r 前置；SEQ_CST store 用 amoswap.rl */
static inline void atomic_store_1(volatile void *p, uint8_t v, int mo) {
    if (mo == ATOMIC_SEQ_CST) {
        __asm__ __volatile__("amoswap.w.rl x0, %0, (%1)" :: "r"((uint32_t)v), "r"((uintptr_t)p) : "memory");
    } else {
        if (mo == ATOMIC_RELEASE) __asm__ __volatile__("fence rw,r" ::: "memory");
        __asm__ __volatile__("sb %0, %1" : : "r"(v), "Q"(*(volatile uint8_t*)p) : "memory");
    }
}
static inline void atomic_store_2(volatile void *p, uint16_t v, int mo) {
    if (mo == ATOMIC_SEQ_CST) {
        __asm__ __volatile__("amoswap.w.rl x0, %0, (%1)" :: "r"((uint32_t)v), "r"((uintptr_t)p) : "memory");
    } else {
        if (mo == ATOMIC_RELEASE) __asm__ __volatile__("fence rw,r" ::: "memory");
        __asm__ __volatile__("sh %0, %1" : : "r"(v), "Q"(*(volatile uint16_t*)p) : "memory");
    }
}
static inline void atomic_store_4(volatile void *p, uint32_t v, int mo) {
    if (mo == ATOMIC_SEQ_CST) {
        __asm__ __volatile__("amoswap.w.rl x0, %0, (%1)" :: "r"(v), "r"((uintptr_t)p) : "memory");
    } else {
        if (mo == ATOMIC_RELEASE) __asm__ __volatile__("fence rw,r" ::: "memory");
        __asm__ __volatile__("sw %0, %1" : : "r"(v), "Q"(*(volatile uint32_t*)p) : "memory");
    }
}
static inline void atomic_store_8(volatile void *p, uint64_t v, int mo) {
    if (mo == ATOMIC_SEQ_CST) {
        __asm__ __volatile__("amoswap.d.rl x0, %0, (%1)" :: "r"(v), "r"((uintptr_t)p) : "memory");
    } else {
        if (mo == ATOMIC_RELEASE) __asm__ __volatile__("fence rw,r" ::: "memory");
        __asm__ __volatile__("sd %0, %1" : : "r"(v), "Q"(*(volatile uint64_t*)p) : "memory");
    }
}

/* ---------------- 4/8 字节 AMO 通用模板 ---------------- */
#define _RV_AMO_RMW(SUF, W, TYPE, NAME, AMO, BODY) \
static inline TYPE atomic_fetch_##NAME##_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old; \
    _RV_PRE_SC(mo); \
    __asm__ __volatile__(AMO _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)" \
                         : "=r"(old) : "r"((TYPE)(BODY)), "r"((uintptr_t)p) : "memory"); \
    return old; \
} \
static inline TYPE atomic_##NAME##_fetch_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old; \
    _RV_PRE_SC(mo); \
    __asm__ __volatile__(AMO _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)" \
                         : "=r"(old) : "r"((TYPE)(BODY)), "r"((uintptr_t)p) : "memory"); \
    return (TYPE)(old OP_COMPAT); \
}

/* fetch_add (amoswap 算子: amoadd) */
static inline uint32_t atomic_fetch_add_4(volatile void *p, uint32_t v, int mo) {
    uint32_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoadd.w" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}
static inline uint64_t atomic_fetch_add_8(volatile void *p, uint64_t v, int mo) {
    uint64_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoadd.d" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}
static inline uint32_t atomic_add_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_add_4(p, v, mo) + v; }
static inline uint64_t atomic_add_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_add_8(p, v, mo) + v; }

/* fetch_sub (amoadd -v) */
static inline uint32_t atomic_fetch_sub_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_add_4(p, (uint32_t)(-v), mo); }
static inline uint64_t atomic_fetch_sub_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_add_8(p, (uint64_t)(-v), mo); }
static inline uint32_t atomic_sub_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_sub_4(p, v, mo) - v; }
static inline uint64_t atomic_sub_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_sub_8(p, v, mo) - v; }

/* fetch_and / or / xor */
static inline uint32_t atomic_fetch_and_4(volatile void *p, uint32_t v, int mo) {
    uint32_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoand.w" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}
static inline uint64_t atomic_fetch_and_8(volatile void *p, uint64_t v, int mo) {
    uint64_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoand.d" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}
static inline uint32_t atomic_and_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_and_4(p, v, mo) & v; }
static inline uint64_t atomic_and_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_and_8(p, v, mo) & v; }

static inline uint32_t atomic_fetch_or_4(volatile void *p, uint32_t v, int mo) {
    uint32_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoor.w" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}
static inline uint64_t atomic_fetch_or_8(volatile void *p, uint64_t v, int mo) {
    uint64_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoor.d" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}
static inline uint32_t atomic_or_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_or_4(p, v, mo) | v; }
static inline uint64_t atomic_or_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_or_8(p, v, mo) | v; }

static inline uint32_t atomic_fetch_xor_4(volatile void *p, uint32_t v, int mo) {
    uint32_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoxor.w" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}
static inline uint64_t atomic_fetch_xor_8(volatile void *p, uint64_t v, int mo) {
    uint64_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoxor.d" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}
static inline uint32_t atomic_xor_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_xor_4(p, v, mo) ^ v; }
static inline uint64_t atomic_xor_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_xor_8(p, v, mo) ^ v; }

/* fetch_nand: RISC-V 没有 amonand，用 amoxor + amoand 组合或 CAS 循环 */
static inline uint32_t atomic_fetch_nand_4(volatile void *p, uint32_t v, int mo) {
    uint32_t old = *(volatile uint32_t*)p, neu;
    do {
        neu = ~(old & v);
    } while (!atomic_compare_exchange_4(p, &old, neu, false, mo, mo));
    return old;
}
static inline uint64_t atomic_fetch_nand_8(volatile void *p, uint64_t v, int mo) {
    uint64_t old = *(volatile uint64_t*)p, neu;
    do {
        neu = ~(old & v);
    } while (!atomic_compare_exchange_8(p, &old, neu, false, mo, mo));
    return old;
}
static inline uint32_t atomic_nand_fetch_4(volatile void *p, uint32_t v, int mo) {
    uint32_t old = *(volatile uint32_t*)p, neu;
    do { neu = ~(old & v); }
    while (!atomic_compare_exchange_4(p, &old, neu, false, mo, mo));
    return neu;
}
static inline uint64_t atomic_nand_fetch_8(volatile void *p, uint64_t v, int mo) {
    uint64_t old = *(volatile uint64_t*)p, neu;
    do { neu = ~(old & v); }
    while (!atomic_compare_exchange_8(p, &old, neu, false, mo, mo));
    return neu;
}

/* ---------------- exchange (amoswap) ---------------- */
static inline uint32_t atomic_exchange_4(volatile void *p, uint32_t v, int mo) {
    uint32_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoswap.w" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}
static inline uint64_t atomic_exchange_8(volatile void *p, uint64_t v, int mo) {
    uint64_t old; _RV_PRE_SC(mo);
    __asm__ __volatile__("amoswap.d" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                         : "=r"(old) : "r"(v), "r"((uintptr_t)p) : "memory");
    return old;
}

/* ---------------- compare_exchange (LR/SC) ---------------- */
#define _RV_CAS_W_OR_D(SUF, INSN, TYPE) \
static inline bool atomic_compare_exchange_##SUF(volatile void *p, void *e, TYPE d, \
                                                 bool weak, int sm, int fm) { \
    (void)weak; (void)fm; \
    TYPE exp = *(TYPE*)e, cur, neu = d; long ok; \
    _RV_PRE_SC(sm); \
    __asm__ __volatile__( \
        "1: " #INSN _RV_AMO_SUFFIX(sm) " %0, 0(%4)\n" \
        "   bne  %0, %2, 2f\n" \
        "   sc.w" _RV_AMO_SUFFIX(sm) " %1, %3, 0(%4)\n" \
        "   bnez %1, 1b\n" \
        "2:\n" \
        : "=&r"(cur), "=&r"(ok) \
        : "r"(exp), "r"(neu), "r"((uintptr_t)p) \
        : "memory"); \
    if (cur != exp) { *(TYPE*)e = cur; return false; } \
    return true; \
}

static inline bool atomic_compare_exchange_4(volatile void *p, void *e, uint32_t d,
                                             bool weak, int sm, int fm) {
    (void)weak; (void)fm;
    uint32_t exp = *(uint32_t*)e, cur, neu = d; long ok;
    _RV_PRE_SC(sm);
    __asm__ __volatile__(
        "1: lr.w" _RV_AMO_SUFFIX(sm) " %0, 0(%4)\n"
        "   bne  %0, %2, 2f\n"
        "   sc.w" _RV_AMO_SUFFIX(sm) " %1, %3, 0(%4)\n"
        "   bnez %1, 1b\n"
        "2:\n"
        : "=&r"(cur), "=&r"(ok)
        : "r"(exp), "r"(neu), "r"((uintptr_t)p)
        : "memory");
    if (cur != exp) { *(uint32_t*)e = cur; return false; }
    return true;
}
static inline bool atomic_compare_exchange_8(volatile void *p, void *e, uint64_t d,
                                             bool weak, int sm, int fm) {
    (void)weak; (void)fm;
    uint64_t exp = *(uint64_t*)e, cur, neu = d; long ok;
    _RV_PRE_SC(sm);
    __asm__ __volatile__(
        "1: lr.d" _RV_AMO_SUFFIX(sm) " %0, 0(%4)\n"
        "   bne  %0, %2, 2f\n"
        "   sc.d" _RV_AMO_SUFFIX(sm) " %1, %3, 0(%4)\n"
        "   bnez %1, 1b\n"
        "2:\n"
        : "=&r"(cur), "=&r"(ok)
        : "r"(exp), "r"(neu), "r"((uintptr_t)p)
        : "memory");
    if (cur != exp) { *(uint64_t*)e = cur; return false; }
    return true;
}

/* ---------------- 1/2 字节 RMW 用 LR/SC + 位掩码 ---------------- */
#define _RV_SUBWORD_RMW(SUF, TYPE, BITS, NAME, EXPR) \
static inline TYPE atomic_fetch_##NAME##_##SUF(volatile void *p, TYPE v, int mo) { \
    uintptr_t addr = (uintptr_t)p; \
    uintptr_t aligned = addr & ~3UL; \
    unsigned shift = (unsigned)((addr & 3UL) * 8); \
    uint32_t mask = ((uint32_t)((1ULL << BITS) - 1)) << shift; \
    uint32_t oldw, neww, byte_old; long ok; \
    _RV_PRE_SC(mo); \
    __asm__ __volatile__( \
        "1: lr.w" _RV_AMO_SUFFIX(mo) " %0, 0(%5)\n" \
        "   and  %1, %0, %3\n" \
        "   srl  %1, %1, %4\n"      /* byte_old = (oldw & mask) >> shift */ \
        "   andi %1, %1, 0xff\n" \
        "   /* compute neww in C below */ \n" \
        "2: sc.w" _RV_AMO_SUFFIX(mo) " %2, %6, 0(%5)\n" \
        "   bnez %2, 1b\n" \
        : "=&r"(oldw), "=&r"(byte_old), "=&r"(ok) \
        : "r"(mask), "r"((uint32_t)shift), "r"(aligned), "r"(neww) \
        : "memory"); \
    /* Above is template; real impl in macros below. */ \
    return (TYPE)byte_old; \
}

/* 由于内联汇编中直接做算术比较复杂，下面给出 1/2 字节 fetch_add 的完整实现示例 */
static inline uint8_t atomic_fetch_add_1(volatile void *p, uint8_t v, int mo) {
    uintptr_t addr = (uintptr_t)p;
    uintptr_t aligned = addr & ~3UL;
    unsigned shift = (unsigned)((addr & 3UL) * 8);
    uint32_t mask = 0xffU << shift;
    uint32_t oldw, neww, byte_old, byte_new; long ok;
    _RV_PRE_SC(mo);
    do {
        __asm__ __volatile__("lr.w" _RV_AMO_SUFFIX(mo) " %0, 0(%1)"
                             : "=r"(oldw) : "r"(aligned) : "memory");
        byte_old = (oldw & mask) >> shift;
        byte_new = (byte_old + v) & 0xff;
        neww = (oldw & ~mask) | (byte_new << shift);
        __asm__ __volatile__("sc.w" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                             : "=&r"(ok) : "r"(neww), "r"(aligned) : "memory");
    } while (ok);
    return (uint8_t)byte_old;
}
static inline uint8_t atomic_add_fetch_1(volatile void *p, uint8_t v, int mo) {
    return atomic_fetch_add_1(p, v, mo) + v;
}
static inline uint8_t atomic_fetch_sub_1(volatile void *p, uint8_t v, int mo) { return atomic_fetch_add_1(p, (uint8_t)(-v), mo); }
static inline uint8_t atomic_sub_fetch_1(volatile void *p, uint8_t v, int mo) { return atomic_fetch_sub_1(p, v, mo) - v; }

static inline uint16_t atomic_fetch_add_2(volatile void *p, uint16_t v, int mo) {
    uintptr_t addr = (uintptr_t)p;
    uintptr_t aligned = addr & ~3UL;
    unsigned shift = (unsigned)((addr & 3UL) * 8);
    uint32_t mask = 0xffffU << shift;
    uint32_t oldw, neww, byte_old, byte_new; long ok;
    _RV_PRE_SC(mo);
    do {
        __asm__ __volatile__("lr.w" _RV_AMO_SUFFIX(mo) " %0, 0(%1)"
                             : "=r"(oldw) : "r"(aligned) : "memory");
        byte_old = (oldw & mask) >> shift;
        byte_new = (byte_old + v) & 0xffff;
        neww = (oldw & ~mask) | (byte_new << shift);
        __asm__ __volatile__("sc.w" _RV_AMO_SUFFIX(mo) " %0, %1, 0(%2)"
                             : "=&r"(ok) : "r"(neww), "r"(aligned) : "memory");
    } while (ok);
    return (uint16_t)byte_old;
}
static inline uint16_t atomic_add_fetch_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_add_2(p, v, mo) + v; }
static inline uint16_t atomic_fetch_sub_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_add_2(p, (uint16_t)(-v), mo); }
static inline uint16_t atomic_sub_fetch_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_sub_2(p, v, mo) - v; }

/* 1/2 字节 and/or/xor/nand/exchange/CAS：可类似用 LR/SC+掩码实现，
   这里用基于 4 字节 CAS 的封装以节省篇幅 */
#define _RV_SUBWORD_BITOP(SUF, TYPE, NAME, OP) \
static inline TYPE atomic_fetch_##NAME##_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old = *(volatile TYPE*)p, neu; \
    do { neu = (TYPE)(old OP v); } \
    while (!atomic_compare_exchange_##SUF(p, &old, neu, false, mo, mo)); \
    return old; \
} \
static inline TYPE atomic_##NAME##_fetch_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old = *(volatile TYPE*)p, neu; \
    do { neu = (TYPE)(old OP v); } \
    while (!atomic_compare_exchange_##SUF(p, &old, neu, false, mo, mo)); \
    return neu; \
}

/* 1/2 字节 CAS（基于 4 字节 LR/SC，掩码比较） */
static inline bool atomic_compare_exchange_1(volatile void *p, void *e, uint8_t d,
                                             bool weak, int sm, int fm) {
    (void)weak; (void)fm;
    uintptr_t addr = (uintptr_t)p;
    uintptr_t aligned = addr & ~3UL;
    unsigned shift = (unsigned)((addr & 3UL) * 8);
    uint32_t mask = 0xffU << shift;
    uint8_t  exp8 = *(uint8_t*)e;
    uint32_t expw_part = (uint32_t)exp8 << shift;
    uint32_t desw_part = (uint32_t)d << shift;
    uint32_t oldw, neww; long ok;
    _RV_PRE_SC(sm);
    do {
        __asm__ __volatile__("lr.w" _RV_AMO_SUFFIX(sm) " %0, 0(%1)"
                             : "=r"(oldw) : "r"(aligned) : "memory");
        if ((oldw & mask) != expw_part) {
            *(uint8_t*)e = (uint8_t)((oldw & mask) >> shift);
            return false;
        }
        neww = (oldw & ~mask) | desw_part;
        __asm__ __volatile__("sc.w" _RV_AMO_SUFFIX(sm) " %0, %1, 0(%2)"
                             : "=&r"(ok) : "r"(neww), "r"(aligned) : "memory");
    } while (ok);
    return true;
}
static inline bool atomic_compare_exchange_2(volatile void *p, void *e, uint16_t d,
                                             bool weak, int sm, int fm) {
    (void)weak; (void)fm;
    uintptr_t addr = (uintptr_t)p;
    uintptr_t aligned = addr & ~3UL;
    unsigned shift = (unsigned)((addr & 3UL) * 8);
    uint32_t mask = 0xffffU << shift;
    uint16_t exp16 = *(uint16_t*)e;
    uint32_t expw_part = (uint32_t)exp16 << shift;
    uint32_t desw_part = (uint32_t)d << shift;
    uint32_t oldw, neww; long ok;
    _RV_PRE_SC(sm);
    do {
        __asm__ __volatile__("lr.w" _RV_AMO_SUFFIX(sm) " %0, 0(%1)"
                             : "=r"(oldw) : "r"(aligned) : "memory");
        if ((oldw & mask) != expw_part) {
            *(uint16_t*)e = (uint16_t)((oldw & mask) >> shift);
            return false;
        }
        neww = (oldw & ~mask) | desw_part;
        __asm__ __volatile__("sc.w" _RV_AMO_SUFFIX(sm) " %0, %1, 0(%2)"
                             : "=&r"(ok) : "r"(neww), "r"(aligned) : "memory");
    } while (ok);
    return true;
}

_RV_SUBWORD_BITOP(1, uint8_t,  and, &)
_RV_SUBWORD_BITOP(2, uint16_t, and, &)
_RV_SUBWORD_BITOP(1, uint8_t,  or,  |)
_RV_SUBWORD_BITOP(2, uint16_t, or,  |)
_RV_SUBWORD_BITOP(1, uint8_t,  xor, ^)
_RV_SUBWORD_BITOP(2, uint16_t, xor, ^)
_RV_SUBWORD_BITOP(1, uint8_t,  nand, &)  /* nand 单独处理 */

/* nand 的旧值是 old，新值是 ~(old & v)，OP 宏不适用，单独写 */
static inline uint8_t atomic_fetch_nand_1(volatile void *p, uint8_t v, int mo) {
    uint8_t old = *(volatile uint8_t*)p, neu;
    do { neu = (uint8_t)~(old & v); }
    while (!atomic_compare_exchange_1(p, &old, neu, false, mo, mo));
    return old;
}
static inline uint16_t atomic_fetch_nand_2(volatile void *p, uint16_t v, int mo) {
    uint16_t old = *(volatile uint16_t*)p, neu;
    do { neu = (uint16_t)~(old & v); }
    while (!atomic_compare_exchange_2(p, &old, neu, false, mo, mo));
    return old;
}
static inline uint8_t atomic_nand_fetch_1(volatile void *p, uint8_t v, int mo) {
    uint8_t old = *(volatile uint8_t*)p, neu;
    do { neu = (uint8_t)~(old & v); }
    while (!atomic_compare_exchange_1(p, &old, neu, false, mo, mo));
    return neu;
}
static inline uint16_t atomic_nand_fetch_2(volatile void *p, uint16_t v, int mo) {
    uint16_t old = *(volatile uint16_t*)p, neu;
    do { neu = (uint16_t)~(old & v); }
    while (!atomic_compare_exchange_2(p, &old, neu, false, mo, mo));
    return neu;
}

/* 1/2 字节 exchange */
static inline uint8_t atomic_exchange_1(volatile void *p, uint8_t v, int mo) {
    uint8_t old = *(volatile uint8_t*)p;
    do { } while (!atomic_compare_exchange_1(p, &old, v, false, mo, mo));
    return old;
}
static inline uint16_t atomic_exchange_2(volatile void *p, uint16_t v, int mo) {
    uint16_t old = *(volatile uint16_t*)p;
    do { } while (!atomic_compare_exchange_2(p, &old, v, false, mo, mo));
    return old;
}

#undef _RV_SUBWORD_BITOP

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