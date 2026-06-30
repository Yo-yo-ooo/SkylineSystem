/*
* SPDX-License-Identifier: MIT
* File: atomic_aarch64.h
* Copyright (C) 2026 Yo-yo-ooo
*/
#ifndef ATOMIC_AARCH64_H
#define ATOMIC_AARCH64_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

/* ---------------- load ---------------- */
// INSN_SUF: "b", "h", ""  REG: "w" (32位), "" (64位)
#define _AARCH64_LOAD(NAME, TYPE, INSN_SUF, REG) \
static inline TYPE atomic_load_##NAME(const volatile void *p, int mo) { \
    TYPE v; \
    if (mo == ATOMIC_RELAXED) \
        __asm__ __volatile__("ldr" INSN_SUF " %" REG "0, %1" : "=r"(v) : "Q"(*(const volatile TYPE*)p)); \
    else \
        __asm__ __volatile__("ldar" INSN_SUF " %" REG "0, %1" : "=r"(v) : "Q"(*(const volatile TYPE*)p) : "memory"); \
    return v; \
}

_AARCH64_LOAD(1, uint8_t,  "b", "w")
_AARCH64_LOAD(2, uint16_t, "h", "w")
_AARCH64_LOAD(4, uint32_t, "",  "w")
_AARCH64_LOAD(8, uint64_t, "",  "")
#undef _AARCH64_LOAD

/* ---------------- store ---------------- */
#define _AARCH64_STORE(NAME, TYPE, INSN_SUF, REG) \
static inline void atomic_store_##NAME(volatile void *p, TYPE v, int mo) { \
    if (mo == ATOMIC_RELAXED) \
        __asm__ __volatile__("str" INSN_SUF " %" REG "0, %1" : : "r"(v), "Q"(*(volatile TYPE*)p)); \
    else \
        __asm__ __volatile__("stlr" INSN_SUF " %" REG "0, %1" : : "r"(v), "Q"(*(volatile TYPE*)p) : "memory"); \
}

_AARCH64_STORE(1, uint8_t,  "b", "w")
_AARCH64_STORE(2, uint16_t, "h", "w")
_AARCH64_STORE(4, uint32_t, "",  "w")
_AARCH64_STORE(8, uint64_t, "",  "")
#undef _AARCH64_STORE

/* ---------------- exchange ---------------- */
#define _AARCH64_EXCHANGE(NAME, TYPE, INSN_SUF, REG) \
static inline TYPE atomic_exchange_##NAME(volatile void *p, TYPE v, int mo) { \
    TYPE old; int ok; \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    do { \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__("ldxr" INSN_SUF " %" REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("ldaxr" INSN_SUF " %" REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p) : "memory"); \
        if (mo == ATOMIC_RELAXED || mo == ATOMIC_ACQUIRE) \
            __asm__ __volatile__("stxr" INSN_SUF " %w0, %" REG "1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("stlxr" INSN_SUF " %w0, %" REG "1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile TYPE*)p) : "memory"); \
    } while (ok); \
    return old; \
}

_AARCH64_EXCHANGE(1, uint8_t,  "b", "w")
_AARCH64_EXCHANGE(2, uint16_t, "h", "w")
_AARCH64_EXCHANGE(4, uint32_t, "",  "w")
_AARCH64_EXCHANGE(8, uint64_t, "",  "")
#undef _AARCH64_EXCHANGE

/* ---------------- compare_exchange ---------------- */
#define _AARCH64_CAS(NAME, TYPE, INSN_SUF, REG) \
static inline bool atomic_compare_exchange_##NAME(volatile void *p, void *e, TYPE d, \
                                                 bool weak, int sm, int fm) { \
    (void)fm; \
    TYPE exp = *(TYPE*)e, cur; int ok = 1; \
    if (sm == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    do { \
        if (sm == ATOMIC_RELAXED) \
            __asm__ __volatile__("ldxr" INSN_SUF " %" REG "0, %1" : "=r"(cur) : "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("ldaxr" INSN_SUF " %" REG "0, %1" : "=r"(cur) : "Q"(*(volatile TYPE*)p) : "memory"); \
        if (cur != exp) { *(TYPE*)e = cur; return false; } \
        if (sm == ATOMIC_RELAXED || sm == ATOMIC_ACQUIRE) \
            __asm__ __volatile__("stxr" INSN_SUF " %w0, %" REG "1, %2" : "=&r"(ok) : "r"(d), "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("stlxr" INSN_SUF " %w0, %" REG "1, %2" : "=&r"(ok) : "r"(d), "Q"(*(volatile TYPE*)p) : "memory"); \
        if (ok && weak) return false; /* 允许伪失败 */ \
    } while (ok); \
    return true; \
}

_AARCH64_CAS(1, uint8_t,  "b", "w")
_AARCH64_CAS(2, uint16_t, "h", "w")
_AARCH64_CAS(4, uint32_t, "",  "w")
_AARCH64_CAS(8, uint64_t, "",  "")
#undef _AARCH64_CAS

/* ---------------- 通用 RMW 模板 ---------------- */
#define _AARCH64_RMW(NAME, TYPE, OP, INSN_SUF, REG, EXPR) \
static inline TYPE atomic_fetch_##OP##_##NAME(volatile void *p, TYPE v, int mo) { \
    TYPE old, neu; int ok; \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    do { \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__("ldxr" INSN_SUF " %" REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("ldaxr" INSN_SUF " %" REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p) : "memory"); \
        neu = (TYPE)(EXPR); \
        if (mo == ATOMIC_RELAXED || mo == ATOMIC_ACQUIRE) \
            __asm__ __volatile__("stxr" INSN_SUF " %w0, %" REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("stlxr" INSN_SUF " %w0, %" REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p) : "memory"); \
    } while (ok); \
    return old; \
} \
static inline TYPE atomic_##OP##_fetch_##NAME(volatile void *p, TYPE v, int mo) { \
    TYPE old, neu; int ok; \
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory"); \
    do { \
        if (mo == ATOMIC_RELAXED) \
            __asm__ __volatile__("ldxr" INSN_SUF " %" REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("ldaxr" INSN_SUF " %" REG "0, %1" : "=r"(old) : "Q"(*(volatile TYPE*)p) : "memory"); \
        neu = (TYPE)(EXPR); \
        if (mo == ATOMIC_RELAXED || mo == ATOMIC_ACQUIRE) \
            __asm__ __volatile__("stxr" INSN_SUF " %w0, %" REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p)); \
        else \
            __asm__ __volatile__("stlxr" INSN_SUF " %w0, %" REG "1, %2" : "=&r"(ok) : "r"(neu), "Q"(*(volatile TYPE*)p) : "memory"); \
    } while (ok); \
    return neu; \
}

_AARCH64_RMW(1, uint8_t,  add, "b", "w", (uint8_t)(old + v))
_AARCH64_RMW(2, uint16_t, add, "h", "w", (uint16_t)(old + v))
_AARCH64_RMW(4, uint32_t, add, "",  "w", (uint32_t)(old + v))
_AARCH64_RMW(8, uint64_t, add, "",  "",  (uint64_t)(old + v))

_AARCH64_RMW(1, uint8_t,  sub, "b", "w", (uint8_t)(old - v))
_AARCH64_RMW(2, uint16_t, sub, "h", "w", (uint16_t)(old - v))
_AARCH64_RMW(4, uint32_t, sub, "",  "w", (uint32_t)(old - v))
_AARCH64_RMW(8, uint64_t, sub, "",  "",  (uint64_t)(old - v))

_AARCH64_RMW(1, uint8_t,  and, "b", "w", (uint8_t)(old & v))
_AARCH64_RMW(2, uint16_t, and, "h", "w", (uint16_t)(old & v))
_AARCH64_RMW(4, uint32_t, and, "",  "w", (uint32_t)(old & v))
_AARCH64_RMW(8, uint64_t, and, "",  "",  (uint64_t)(old & v))

_AARCH64_RMW(1, uint8_t,  or,  "b", "w", (uint8_t)(old | v))
_AARCH64_RMW(2, uint16_t, or,  "h", "w", (uint16_t)(old | v))
_AARCH64_RMW(4, uint32_t, or,  "",  "w", (uint32_t)(old | v))
_AARCH64_RMW(8, uint64_t, or,  "",  "",  (uint64_t)(old | v))

_AARCH64_RMW(1, uint8_t,  xor, "b", "w", (uint8_t)(old ^ v))
_AARCH64_RMW(2, uint16_t, xor, "h", "w", (uint16_t)(old ^ v))
_AARCH64_RMW(4, uint32_t, xor, "",  "w", (uint32_t)(old ^ v))
_AARCH64_RMW(8, uint64_t, xor, "",  "",  (uint64_t)(old ^ v))

_AARCH64_RMW(1, uint8_t,  nand, "b", "w", (uint8_t)~(old & v))
_AARCH64_RMW(2, uint16_t, nand, "h", "w", (uint16_t)~(old & v))
_AARCH64_RMW(4, uint32_t, nand, "",  "w", (uint32_t)~(old & v))
_AARCH64_RMW(8, uint64_t, nand, "",  "",  (uint64_t)~(old & v))

#undef _AARCH64_RMW

/* ---------------- test_and_set / clear ---------------- */
static inline bool atomic_test_and_set(volatile void *p, int mo) {
    uint8_t v = 1, old; int ok;
    if (mo == ATOMIC_SEQ_CST) __asm__ __volatile__("dmb ish" ::: "memory");
    do {
        if (mo == ATOMIC_RELAXED) __asm__ __volatile__("ldxrb %w0, %1" : "=r"(old) : "Q"(*(volatile uint8_t*)p));
        else                      __asm__ __volatile__("ldaxrb %w0, %1" : "=r"(old) : "Q"(*(volatile uint8_t*)p) : "memory");
        if (mo == ATOMIC_RELAXED || mo == ATOMIC_ACQUIRE) 
            __asm__ __volatile__("stxrb %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint8_t*)p));
        else                      
            __asm__ __volatile__("stlxrb %w0, %w1, %2" : "=&r"(ok) : "r"(v), "Q"(*(volatile uint8_t*)p) : "memory");
    } while (ok);
    return old != 0;
}

static inline void atomic_clear(volatile void *p, int mo) {
    atomic_store_1(p, 0, mo);
}

static inline bool atomic_is_lock_free(size_t n)    { return n <= 8; }
static inline bool atomic_always_lock_free(size_t n){ return n <= 8; }

#endif /* ATOMIC_AARCH64_H */