/*
* SPDX-License-Identifier: MIT
* File: atomic_aarch64.h
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
#ifndef ATOMIC_AARCH64_H
#define ATOMIC_AARCH64_H

#define _ATOMIC_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* ---------------- fence ---------------- */
static inline void atomic_thread_fence(int mo) {
    switch (mo) {
    case ATOMIC_RELAXED: break;
    case ATOMIC_CONSUME:
    case ATOMIC_ACQUIRE: __asm__ __volatile__("dmb ishld" ::: "memory"); break;
    case ATOMIC_RELEASE: __asm__ __volatile__("dmb ishst" ::: "memory"); break;
    case ATOMIC_ACQ_REL:
    case ATOMIC_SEQ_CST: __asm__ __volatile__("dmb ish" ::: "memory"); break;
    }
}
static inline void atomic_signal_fence(int mo) { (void)mo; _ATOMIC_COMPILER_BARRIER(); }

/* SEQ_CST RMW 需要的前置/后置屏障 */
#define _AARCH64_PRE_SC()  do { if (0) {} } while (0)
#define _AARCH64_POST_SC() __asm__ __volatile__("dmb ish" ::: "memory")

/* ---------------- load (LDAR 提供 acquire；SEQ_CST 也用 LDAR) ---------------- */
static inline uint8_t atomic_load_1(const volatile void *p, int mo) {
    uint8_t v;
    if (mo == ATOMIC_RELAXED)
        __asm__ __volatile__("ldrb %w0, %1" : "=r"(v) : "Q"(*(const volatile uint8_t*)p) : "memory");
    else
        __asm__ __volatile__("ldarb %w0, %1" : "=r"(v) : "Q"(*(const volatile uint8_t*)p) : "memory");
    return v;
}
static inline uint16_t atomic_load_2(const volatile void *p, int mo) {
    uint16_t v;
    if (mo == ATOMIC_RELAXED)
        __asm__ __volatile__("ldrh %w0, %1" : "=r"(v) : "Q"(*(const volatile uint16_t*)p) : "memory");
    else
        __asm__ __volatile__("ldarh %w0, %1" : "=r"(v) : "Q"(*(const volatile uint16_t*)p) : "memory");
    return v;
}
static inline uint32_t atomic_load_4(const volatile void *p, int mo) {
    uint32_t v;
    if (mo == ATOMIC_RELAXED)
        __asm__ __volatile__("ldr %w0, %1" : "=r"(v) : "Q"(*(const volatile uint32_t*)p) : "memory");
    else
        __asm__ __volatile__("ldar %w0, %1" : "=r"(v) : "Q"(*(const volatile uint32_t*)p) : "memory");
    return v;
}
static inline uint64_t atomic_load_8(const volatile void *p, int mo) {
    uint64_t v;
    if (mo == ATOMIC_RELAXED)
        __asm__ __volatile__("ldr %0, %1" : "=r"(v) : "Q"(*(const volatile uint64_t*)p) : "memory");
    else
        __asm__ __volatile__("ldar %0, %1" : "=r"(v) : "Q"(*(const volatile uint64_t*)p) : "memory");
    return v;
}

/* ---------------- store (STLR 提供 release；SEQ_CST 也用 STLR) ---------------- */
static inline void atomic_store_1(volatile void *p, uint8_t v, int mo) {
    if (mo == ATOMIC_RELAXED)
        __asm__ __volatile__("strb %w0, %1" : : "r"(v), "Q"(*(volatile uint8_t*)p) : "memory");
    else
        __asm__ __volatile__("stlrb %w0, %1" : : "r"(v), "Q"(*(volatile uint8_t*)p) : "memory");
}
static inline void atomic_store_2(volatile void *p, uint16_t v, int mo) {
    if (mo == ATOMIC_RELAXED)
        __asm__ __volatile__("strh %w0, %1" : : "r"(v), "Q"(*(volatile uint16_t*)p) : "memory");
    else
        __asm__ __volatile__("stlrh %w0, %1" : : "r"(v), "Q"(*(volatile uint16_t*)p) : "memory");
}
static inline void atomic_store_4(volatile void *p, uint32_t v, int mo) {
    if (mo == ATOMIC_RELAXED)
        __asm__ __volatile__("str %w0, %1" : : "r"(v), "Q"(*(volatile uint32_t*)p) : "memory");
    else
        __asm__ __volatile__("stlr %w0, %1" : : "r"(v), "Q"(*(volatile uint32_t*)p) : "memory");
}
static inline void atomic_store_8(volatile void *p, uint64_t v, int mo) {
    if (mo == ATOMIC_RELAXED)
        __asm__ __volatile__("str %0, %1" : : "r"(v), "Q"(*(volatile uint64_t*)p) : "memory");
    else
        __asm__ __volatile__("stlr %0, %1" : : "r"(v), "Q"(*(volatile uint64_t*)p) : "memory");
}

/* ---------------- exchange (LDXR/STXR 循环) ---------------- */
#define _AARCH64_EXCHANGE(SUF, REG, TYPE) \
static inline TYPE atomic_exchange_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old; int ok; \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    do { \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__("ldxr" #SUF " %" #REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("ldaxr" #SUF " %" #REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p)); \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__("stxr" #SUF " %w0, %" #REG "1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("stlxr" #SUF " %w0, %" #REG "1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile TYPE*)p)); \
    } while (ok); \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    return old; \
}

/* 使用 ldaxr/stlxr 自带 acquire/release；SEQ_CST 加 dmb ish */
static inline uint8_t atomic_exchange_1(volatile void *p, uint8_t v, int mo) {
    uint8_t old; int ok;
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    do {
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("ldxrb %w0, %1" : "=r"(old) : "Q"(*(volatile uint8_t*)p));
        else                      __asm__ __volatile__("ldaxrb %w0, %1" : "=r"(old) : "Q"(*(volatile uint8_t*)p));
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("stxrb %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint8_t*)p));
        else                      __asm__ __volatile__("stlxrb %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint8_t*)p));
    } while (ok);
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    return old;
}
static inline uint16_t atomic_exchange_2(volatile void *p, uint16_t v, int mo) {
    uint16_t old; int ok;
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    do {
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("ldxrh %w0, %1" : "=r"(old) : "Q"(*(volatile uint16_t*)p));
        else                      __asm__ __volatile__("ldaxrh %w0, %1" : "=r"(old) : "Q"(*(volatile uint16_t*)p));
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("stxrh %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint16_t*)p));
        else                      __asm__ __volatile__("stlxrh %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint16_t*)p));
    } while (ok);
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    return old;
}
static inline uint32_t atomic_exchange_4(volatile void *p, uint32_t v, int mo) {
    uint32_t old; int ok;
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    do {
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("ldxr %w0, %1" : "=r"(old) : "Q"(*(volatile uint32_t*)p));
        else                      __asm__ __volatile__("ldaxr %w0, %1" : "=r"(old) : "Q"(*(volatile uint32_t*)p));
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("stxr %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint32_t*)p));
        else                      __asm__ __volatile__("stlxr %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint32_t*)p));
    } while (ok);
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    return old;
}
static inline uint64_t atomic_exchange_8(volatile void *p, uint64_t v, int mo) {
    uint64_t old; int ok;
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    do {
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("ldxr %0, %1" : "=r"(old) : "Q"(*(volatile uint64_t*)p));
        else                      __asm__ __volatile__("ldaxr %0, %1" : "=r"(old) : "Q"(*(volatile uint64_t*)p));
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("stxr %w0, %1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint64_t*)p));
        else                      __asm__ __volatile__("stlxr %w0, %1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint64_t*)p));
    } while (ok);
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    return old;
}

/* ---------------- compare_exchange (LDXR/STXR 循环) ---------------- */
#define _AARCH64_CAS(SUF, REG, TYPE) \
static inline bool atomic_compare_exchange_##SUF(volatile void *p, void *e, TYPE d, \
                                                 bool weak, int sm, int fm) { \
    (void)weak; (void)fm; \
    TYPE exp = *(TYPE*)e, cur, neu = d; int ok; \
    if (sm == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    do { \
        if (sm == ATOMIC_RELAXED) \
            __asm__ __volatile__("ldxr" #SUF " %" #REG "0, %1" : "=r"(cur) : "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("ldaxr" #SUF " %" #REG "0, %1" : "=r"(cur) : "Q"(*(volatile TYPE*)p)); \
        if (cur != exp) { *(TYPE*)e = cur; break; } \
        if (sm == ATOMIC_RELAXED) \
            __asm__ __volatile__("stxr" #SUF " %w0, %" #REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("stlxr" #SUF " %w0, %" #REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p)); \
    } while (ok); \
    if (sm == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    return cur == exp; \
}

_AARCH64_CAS(b, w, uint8_t)
_AARCH64_CAS(h, w, uint16_t)
_AARCH64_CAS(,  w, uint32_t)
_AARCH64_CAS(,  x, uint64_t)
#undef _AARCH64_CAS

/* ---------------- 通用 RMW 模板 (LDXR/STXR) ---------------- */
#define _AARCH64_RMW(SUF, REG, TYPE, NAME, BODY) \
static inline TYPE atomic_fetch_##NAME##_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old, neu; int ok; \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    do { \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__("ldxr" #SUF " %" #REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("ldaxr" #SUF " %" #REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p)); \
        neu = (BODY); \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__("stxr" #SUF " %w0, %" #REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("stlxr" #SUF " %w0, %" #REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p)); \
    } while (ok); \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    return old; \
} \
static inline TYPE atomic_##NAME##_fetch_##SUF(volatile void *p, TYPE v, int mo) { \
    TYPE old, neu; int ok; \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    do { \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__("ldxr" #SUF " %" #REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("ldaxr" #SUF " %" #REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p)); \
        neu = (BODY); \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__("stxr" #SUF " %w0, %" #REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("stlxr" #SUF " %w0, %" #REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p)); \
    } while (ok); \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    return neu; \
}

_AARCH64_RMW(b, w, uint8_t,  add, (uint8_t)(old + v))
_AARCH64_RMW(h, w, uint16_t, add, (uint16_t)(old + v))
_AARCH64_RMW(,  w, uint32_t, add, (uint32_t)(old + v))
_AARCH64_RMW(,  x, uint64_t, add, (uint64_t)(old + v))

_AARCH64_RMW(b, w, uint8_t,  sub, (uint8_t)(old - v))
_AARCH64_RMW(h, w, uint16_t, sub, (uint16_t)(old - v))
_AARCH64_RMW(,  w, uint32_t, sub, (uint32_t)(old - v))
_AARCH64_RMW(,  x, uint64_t, sub, (uint64_t)(old - v))

_AARCH64_RMW(b, w, uint8_t,  and, (uint8_t)(old & v))
_AARCH64_RMW(h, w, uint16_t, and, (uint16_t)(old & v))
_AARCH64_RMW(,  w, uint32_t, and, (uint32_t)(old & v))
_AARCH64_RMW(,  x, uint64_t, and, (uint64_t)(old & v))

_AARCH64_RMW(b, w, uint8_t,  or,  (uint8_t)(old | v))
_AARCH64_RMW(h, w, uint16_t, or,  (uint16_t)(old | v))
_AARCH64_RMW(,  w, uint32_t, or,  (uint32_t)(old | v))
_AARCH64_RMW(,  x, uint64_t, or,  (uint64_t)(old | v))

_AARCH64_RMW(b, w, uint8_t,  xor, (uint8_t)(old ^ v))
_AARCH64_RMW(h, w, uint16_t, xor, (uint16_t)(old ^ v))
_AARCH64_RMW(,  w, uint32_t, xor, (uint32_t)(old ^ v))
_AARCH64_RMW(,  x, uint64_t, xor, (uint64_t)(old ^ v))

_AARCH64_RMW(b, w, uint8_t,  nand, (uint8_t)~(old & v))
_AARCH64_RMW(h, w, uint16_t, nand, (uint16_t)~(old & v))
_AARCH64_RMW(,  w, uint32_t, nand, (uint32_t)~(old & v))
_AARCH64_RMW(,  x, uint64_t, nand, (uint64_t)~(old & v))

#undef _AARCH64_RMW

/* ---------------- test_and_set / clear ---------------- */
static inline bool atomic_test_and_set(volatile void *p, int mo) {
    uint8_t v = 1, old; int ok;
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    do {
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("ldxrb %w0, %1" : "=r"(old) : "Q"(*(volatile uint8_t*)p));
        else                      __asm__ __volatile__("ldaxrb %w0, %1" : "=r"(old) : "Q"(*(volatile uint8_t*)p));
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("stxrb %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint8_t*)p));
        else                      __asm__ __volatile__("stlxrb %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint8_t*)p));
    } while (ok);
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    return old != 0;
}
static inline void atomic_clear(volatile void *p, int mo) {
    atomic_store_1(p, 0, mo);
}

static inline bool atomic_is_lock_free(size_t n)    { return n <= 8; }
static inline bool atomic_always_lock_free(size_t n){ return n <= 8; }

#endif /* ATOMIC_AARCH64_H */