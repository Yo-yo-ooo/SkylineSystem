// SPDX-FileCopyrightText: 2026 Yo-yoo-ooo
// SPDX-License-Identifier: MIT
#ifndef _COMMON_H_ 
#define _COMMON_H_
#include <stdint.h>
#include <stddef.h>
#include <atomic/atomic.h>

#define CPU_RELAX() __asm__ __volatile__("pause" ::: "memory")

/*
 * CAS (Compare-And-Swap) for 64-bit unsigned integer.
 * Returns the value that was in memory at the time of the CAS attempt:
 *   - On success: returns 'old' (unchanged, match confirmed)
 *   - On failure: returns the current memory value (loaded into expected)
 */
static inline __attribute__((always_inline)) uint64_t A_CAS_U64(volatile uint64_t *p, uint64_t old, uint64_t new) {
    uint64_t expected = old;
    atomic_compare_exchange_8(
        (volatile void *)p,
        &expected,
        new,
        false,           /* strong (non-weak) CAS */
        ATOMIC_SEQ_CST,  /* success memory order */
        ATOMIC_SEQ_CST); /* failure memory order */
    return expected;
}

/*
 * CAS for pointer-sized values.
 * Same semantics as A_CAS_U64 but for void*.
 * Uses uintptr_t for safe pointer-to-integer conversion.
 */
static inline __attribute__((always_inline)) void *__a_cas_p(volatile void *p, void *t, void *s) {
    uintptr_t expected = (uintptr_t)t;
    uintptr_t desired = (uintptr_t)s;
    atomic_compare_exchange_8(
        (volatile void *)p,
        &expected,
        desired,
        false,
        ATOMIC_SEQ_CST,
        ATOMIC_SEQ_CST);
    return (void *)expected;
}

/*
 * Atomic fetch-and-add for 32-bit unsigned integer.
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint32_t __a_fetch_addu32(volatile uint32_t *p, uint32_t v) {
    return atomic_fetch_add_4((volatile void *)p, v, ATOMIC_SEQ_CST);
}

/*
 * Atomic fetch-and-add for 64-bit unsigned integer.
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint64_t __a_fetch_addu64(volatile uint64_t *p, uint64_t val) {
    return atomic_fetch_add_8((volatile void *)p, val, ATOMIC_SEQ_CST);
}

/*
 * Atomic fetch-and-sub for 64-bit unsigned integer.
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint64_t __a_subu64(volatile uint64_t *p, uint64_t val) {
    return atomic_fetch_sub_8((volatile void *)p, val, ATOMIC_SEQ_CST);
}

/*
 * Atomic fetch-and-sub for 32-bit unsigned integer.
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint32_t __a_subu32(volatile uint32_t *p, uint32_t val) {
    return atomic_fetch_sub_4((volatile void *)p, val, ATOMIC_SEQ_CST);
}

/*
 * Atomic fetch-and-or for 64-bit unsigned integer.
 * Returns the value previously stored at *p.
 *
 * Uses ACQ_REL ordering: the read-modify-write is self-synchronizing,
 * and ACQ_REL is sufficient for typical flag-setting use cases.
 */
static inline __attribute__((always_inline)) uint64_t __a_or_64(volatile uint64_t *p, uint64_t v) {
    return atomic_fetch_or_8((volatile void *)p, v, ATOMIC_ACQ_REL);
}

/*
 * Atomic fetch-and-and for 64-bit unsigned integer.
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint64_t __a_and_64(volatile uint64_t *p, uint64_t v) {
    return atomic_fetch_and_8((volatile void *)p, v, ATOMIC_SEQ_CST);
}

/*
 * Atomic clear-bits for 64-bit unsigned integer.
 * Clears the bits specified by bit_mask (performs *p &= ~bit_mask).
 * Returns the value previously stored at *p.
 */
static inline __attribute__((always_inline)) uint64_t __a_clear_bit(volatile uint64_t *p, uint64_t bit_mask) {
    return atomic_fetch_and_8((volatile void *)p, ~bit_mask, ATOMIC_SEQ_CST);
}

#endif /* _X86_64_ATOMIC_COMMON_H_ */