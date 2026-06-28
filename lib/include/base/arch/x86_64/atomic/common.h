//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#pragma once
#ifndef _x86_64_ATOMIC_COMMON_H_
#define _x86_64_ATOMIC_COMMON_H_

#include <stdint.h>
#include <stddef.h>
#include <atomic/atomic.h>

#define CPU_RELAX() __builtin_ia32_pause()

/*
 * CAS (Compare-And-Swap) for 64-bit unsigned integer.
 * Returns the value that was in memory at the time of the CAS attempt:
 *   - On success: returns 'old' (unchanged, match confirmed)
 *   - On failure: returns the current memory value (loaded into expected)
 *
 * __atomic_compare_exchange_n updates *expected on failure, so returning
 * __old gives the correct semantics matching the original cmpxchgq.
 */
#define A_CAS_U64_ASM(p, old, new) ({                                   \
    uint64_t __old = (old);                                             \
    atomic_compare_exchange_n(                                        \
        (volatile uint64_t *)(p),                                       \
        &__old,                                                         \
        (uint64_t)(new),                                                \
        0,                  /* strong (non-weak) CAS                   */ \
        __ATOMIC_SEQ_CST,   /* success memory order                    */ \
        __ATOMIC_SEQ_CST);  /* failure memory order                    */ \
    __old;                                                              \
})

/*
 * CAS for pointer-sized values.
 * Same semantics as A_CAS_U64_ASM but for void*.
 */
static inline __attribute__((always_inline)) void *__a_cas_p(volatile void *p, void *t, void *s)
{
    void *expected = t;
    atomic_compare_exchange_n(
        (void *volatile *)p,
        &expected,
        (void *)(s),
        0,
        __ATOMIC_SEQ_CST,
        __ATOMIC_SEQ_CST);
    return expected;
}

/*
 * Atomic fetch-and-add for 32-bit unsigned integer.
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint32_t __a_fetch_addu32(volatile uint32_t *p, uint32_t v)
{
    return atomic_fetch_add_4(p, v, __ATOMIC_SEQ_CST);
}

/*
 * Atomic fetch-and-add for 64-bit unsigned integer.
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint64_t __a_fetch_addu64(volatile uint64_t *p, uint64_t val)
{
    return atomic_fetch_add_8(p, val, __ATOMIC_SEQ_CST);
}

/*
 * Atomic fetch-and-sub for 64-bit unsigned integer (macro version).
 * Returns the value previously stored at *p.
 */
#define A_FETCH_SUB_U64(p, val) ({                                      \
    atomic_fetch_sub_n((volatile uint64_t *)(p),                        \
                       (uint64_t)(val),                                 \
                       __ATOMIC_SEQ_CST);                               \
})

/*
 * Atomic fetch-and-sub for 64-bit unsigned integer (function version).
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint64_t __a_subu64(volatile uint64_t *p, uint64_t val)
{
    return atomic_fetch_sub_8(p, val, __ATOMIC_SEQ_CST);
}

/*
 * Atomic fetch-and-sub for 32-bit unsigned integer.
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint32_t __a_subu32(volatile uint32_t *p, uint32_t val)
{
    return atomic_fetch_sub_4(p, val, __ATOMIC_SEQ_CST);
}

/*
 * Atomic fetch-and-or for 64-bit unsigned integer.
 * Returns the value previously stored at *p.
 *
 * Uses ACQ_REL ordering: the read-modify-write is self-synchronizing,
 * and ACQ_REL is sufficient for typical flag-setting use cases.
 * (Original code used a CAS loop with the same ACQ_REL/RELAXED ordering.)
 */
static inline __attribute__((always_inline)) uint64_t __a_or_64(volatile uint64_t *p, uint64_t v)
{
    return atomic_fetch_or_8(p, v, __ATOMIC_ACQ_REL);
}

/*
 * Atomic fetch-and-and for 64-bit unsigned integer.
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint64_t __a_and_64(volatile uint64_t *p, uint64_t v)
{
    return atomic_fetch_and_8(p, v, __ATOMIC_SEQ_CST);
}

/*
 * Atomic clear-bits for 64-bit unsigned integer.
 * Clears the bits specified by bit_mask (performs *p &= ~bit_mask).
 * Returns the value previously stored at *p.
 *
 * Equivalent to fetch_and(~bit_mask).
 */
static inline __attribute__((always_inline)) uint64_t __a_clear_bit(volatile uint64_t *p, uint64_t bit_mask)
{
    return atomic_fetch_and_8(p, ~bit_mask, __ATOMIC_SEQ_CST);
}

#endif /* _x86_64_ATOMIC_COMMON_H_ */