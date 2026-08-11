/* stdbit_impl.h — Portable C23 stdbit.h subset implementation
 *
 * Implements:
 *   7.18.13 stdc_has_single_bit[_uc|_us|_ui|_ul|_ull] (and generic)
 *   7.18.14 stdc_bit_width[_uc|_us|_ui|_ul|_ull]      (and generic)
 *   7.18.15 stdc_bit_floor[_uc|_us|_ui|_ul|_ull]      (and generic)
 *   7.18.16 stdc_bit_ceil[_uc|_us|_ui|_ul|_ull]       (and generic)
 *
 * Compile with any C11 (or later) compiler; uses _Generic for the
 * type-generic macros so C11 is required for those. The suffixed
 * functions themselves work in C99.
 */

#ifndef STDBIT_IMPL_H
#define STDBIT_IMPL_H

#include <stdbool.h>
#include <stdint.h>
#include <limits.h>

 /* ------------------------------------------------------------------ */
 /* Internal helpers                                                   */
 /* ------------------------------------------------------------------ */

 /* Count leading zeros for each fixed-width unsigned type.
  * These are the building blocks for everything else. We provide
  * portable (no builtins) and builtin-based variants. */

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)

static inline unsigned int __stdc_clz_uc(unsigned char v)
{
#if defined(__GNUC__) || defined(__clang__)
    if (v == 0) return (unsigned int)CHAR_BIT;
    return (unsigned int)__builtin_clz((unsigned int)v)
        - (unsigned int)(sizeof(unsigned int) * CHAR_BIT - CHAR_BIT);
#else
    unsigned int n = 0;
    unsigned int x = (unsigned int)v << (sizeof(unsigned int) * CHAR_BIT - CHAR_BIT);
    if (x == 0) return (unsigned int)CHAR_BIT;
    while ((x & 0x80000000u) == 0) { x <<= 1; ++n; }
    return n;
#endif
}

static inline unsigned int __stdc_clz_us(unsigned short v)
{
#if defined(__GNUC__) || defined(__clang__)
    if (v == 0) return (unsigned int)(sizeof(unsigned short) * CHAR_BIT);
    return (unsigned int)__builtin_clz((unsigned int)v)
        - (unsigned int)(sizeof(unsigned int) * CHAR_BIT - sizeof(unsigned short) * CHAR_BIT);
#else
    unsigned int n = 0;
    unsigned int x = (unsigned int)v << (sizeof(unsigned int) * CHAR_BIT - sizeof(unsigned short) * CHAR_BIT);
    if (x == 0) return (unsigned int)(sizeof(unsigned short) * CHAR_BIT);
    while ((x & 0x80000000u) == 0) { x <<= 1; ++n; }
    return n;
#endif
}

static inline unsigned int __stdc_clz_ui(unsigned int v)
{
#if defined(__GNUC__) || defined(__clang__)
    if (v == 0) return (unsigned int)(sizeof(unsigned int) * CHAR_BIT);
    return (unsigned int)__builtin_clz(v);
#else
    unsigned int n = 0;
    if (v == 0) return (unsigned int)(sizeof(unsigned int) * CHAR_BIT);
    while ((v & (~0u >> 1 << 1 | (1u << (sizeof(unsigned int) * CHAR_BIT - 1)))) == 0) { /* placeholder */ }
    /* simpler: */
    n = 0;
    while (!(v & (1u << (sizeof(unsigned int) * CHAR_BIT - 1)))) { v <<= 1; ++n; }
    return n;
#endif
}

static inline unsigned int __stdc_clz_ul(unsigned long v)
{
#if defined(__GNUC__) || defined(__clang__)
    if (v == 0) return (unsigned int)(sizeof(unsigned long) * CHAR_BIT);
    return (unsigned int)__builtin_clzl(v);
#else
    unsigned int n = 0;
    if (v == 0) return (unsigned int)(sizeof(unsigned long) * CHAR_BIT);
    while (!(v & (1ul << (sizeof(unsigned long) * CHAR_BIT - 1)))) { v <<= 1; ++n; }
    return n;
#endif
}

static inline unsigned int __stdc_clz_ull(unsigned long long v)
{
#if defined(__GNUC__) || defined(__clang__)
    if (v == 0) return (unsigned int)(sizeof(unsigned long long) * CHAR_BIT);
    return (unsigned int)__builtin_clzll(v);
#else
    unsigned int n = 0;
    if (v == 0) return (unsigned int)(sizeof(unsigned long long) * CHAR_BIT);
    while (!(v & (1ull << (sizeof(unsigned long long) * CHAR_BIT - 1)))) { v <<= 1; ++n; }
    return n;
#endif
}

/* Width of each unsigned type, in bits */
#define __STDBIT_W_UC   (unsigned int)CHAR_BIT
#define __STDBIT_W_US   (unsigned int)(sizeof(unsigned short) * CHAR_BIT)
#define __STDBIT_W_UI   (unsigned int)(sizeof(unsigned int) * CHAR_BIT)
#define __STDBIT_W_UL   (unsigned int)(sizeof(unsigned long) * CHAR_BIT)
#define __STDBIT_W_ULL  (unsigned int)(sizeof(unsigned long long) * CHAR_BIT)

/* ------------------------------------------------------------------ */
/* 7.18.13  Single-bit Check                                          */
/* ------------------------------------------------------------------ */

bool stdc_has_single_bit_uc(unsigned char value)
{
    return value != 0 && (value & (unsigned char)(value - 1)) == 0;
}

bool stdc_has_single_bit_us(unsigned short value)
{
    return value != 0 && (value & (unsigned short)(value - 1)) == 0;
}

bool stdc_has_single_bit_ui(unsigned int value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

bool stdc_has_single_bit_ul(unsigned long int value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

bool stdc_has_single_bit_ull(unsigned long long int value)
{
    return value != 0 && (value & (value - 1)) == 0;
}

/* ------------------------------------------------------------------ */
/* 7.18.14  Bit Width                                                 */
/* ------------------------------------------------------------------ */

unsigned int stdc_bit_width_uc(unsigned char value)
{
    return value == 0 ? 0u : __STDBIT_W_UC - __stdc_clz_uc(value);
}

unsigned int stdc_bit_width_us(unsigned short value)
{
    return value == 0 ? 0u : __STDBIT_W_US - __stdc_clz_us(value);
}

unsigned int stdc_bit_width_ui(unsigned int value)
{
    return value == 0 ? 0u : __STDBIT_W_UI - __stdc_clz_ui(value);
}

unsigned int stdc_bit_width_ul(unsigned long int value)
{
    return value == 0 ? 0u : __STDBIT_W_UL - __stdc_clz_ul(value);
}

unsigned int stdc_bit_width_ull(unsigned long long int value)
{
    return value == 0 ? 0u : __STDBIT_W_ULL - __stdc_clz_ull(value);
}

/* ------------------------------------------------------------------ */
/* 7.18.15  Bit Floor                                                 */
/* ------------------------------------------------------------------ */

unsigned char stdc_bit_floor_uc(unsigned char value)
{
    if (value == 0) return 0;
    unsigned char r = (unsigned char)1
        << (__STDBIT_W_UC - 1u - __stdc_clz_uc(value));
    return r;
}

unsigned short stdc_bit_floor_us(unsigned short value)
{
    if (value == 0) return 0;
    unsigned short r = (unsigned short)1
        << (__STDBIT_W_US - 1u - __stdc_clz_us(value));
    return r;
}

unsigned int stdc_bit_floor_ui(unsigned int value)
{
    if (value == 0) return 0;
    return 1u << (__STDBIT_W_UI - 1u - __stdc_clz_ui(value));
}

unsigned long int stdc_bit_floor_ul(unsigned long int value)
{
    if (value == 0) return 0;
    return 1ul << (__STDBIT_W_UL - 1u - __stdc_clz_ul(value));
}

unsigned long long int stdc_bit_floor_ull(unsigned long long int value)
{
    if (value == 0) return 0;
    return 1ull << (__STDBIT_W_ULL - 1u - __stdc_clz_ull(value));
}

/* ------------------------------------------------------------------ */
/* 7.18.16  Bit Ceiling                                               */
/*                                                                    */
/* If value <= 1, return 1.                                           */
/* Otherwise compute next power of 2 >= value. If the shift would     */
/* overflow (i.e. bit_width(value) == width of the type), return 0.   */
/* ------------------------------------------------------------------ */

unsigned char stdc_bit_ceil_uc(unsigned char value)
{
    if (value <= 1) return (unsigned char)1;
    unsigned int w = stdc_bit_width_uc(value - 1);   /* == bit_width(value-1) */
    if (w >= __STDBIT_W_UC) return 0;                /* does not fit          */
    return (unsigned char)((unsigned char)1 << w);
}

unsigned short stdc_bit_ceil_us(unsigned short value)
{
    if (value <= 1) return (unsigned short)1;
    unsigned int w = stdc_bit_width_us(value - 1);
    if (w >= __STDBIT_W_US) return 0;
    return (unsigned short)((unsigned short)1 << w);
}

unsigned int stdc_bit_ceil_ui(unsigned int value)
{
    if (value <= 1) return 1u;
    unsigned int w = stdc_bit_width_ui(value - 1);
    if (w >= __STDBIT_W_UI) return 0;
    return 1u << w;
}

unsigned long int stdc_bit_ceil_ul(unsigned long int value)
{
    if (value <= 1) return 1ul;
    unsigned int w = stdc_bit_width_ul(value - 1);
    if (w >= __STDBIT_W_UL) return 0;
    return 1ul << w;
}

unsigned long long int stdc_bit_ceil_ull(unsigned long long int value)
{
    if (value <= 1) return 1ull;
    unsigned int w = stdc_bit_width_ull(value - 1);
    if (w >= __STDBIT_W_ULL) return 0;
    return 1ull << w;
}

/* ------------------------------------------------------------------ */
/* Type-generic macros                                                */
/*                                                                    */
/* The standard's generic_return_type is implementation-defined; we   */
/* use unsigned int, which is what glibc/musl use and which is        */
/* always wide enough on real platforms (max result is 64 or 128).    */
/* ------------------------------------------------------------------ */

#define stdc_has_single_bit(value) \
    _Generic((value),                                              \
        unsigned char:        stdc_has_single_bit_uc,              \
        unsigned short:       stdc_has_single_bit_us,              \
        unsigned int:         stdc_has_single_bit_ui,              \
        unsigned long:        stdc_has_single_bit_ul,              \
        unsigned long long:   stdc_has_single_bit_ull,             \
        default:              stdc_has_single_bit_ui)((value))

#define stdc_bit_width(value) \
    _Generic((value),                                              \
        unsigned char:        stdc_bit_width_uc,                   \
        unsigned short:       stdc_bit_width_us,                   \
        unsigned int:         stdc_bit_width_ui,                   \
        unsigned long:        stdc_bit_width_ul,                   \
        unsigned long long:   stdc_bit_width_ull,                  \
        default:              stdc_bit_width_ui)((value))

#define stdc_bit_floor(value) \
    _Generic((value),                                              \
        unsigned char:        stdc_bit_floor_uc,                   \
        unsigned short:       stdc_bit_floor_us,                   \
        unsigned int:         stdc_bit_floor_ui,                   \
        unsigned long:        stdc_bit_floor_ul,                   \
        unsigned long long:   stdc_bit_floor_ull,                  \
        default:              stdc_bit_floor_ui)((value))

#define stdc_bit_ceil(value) \
    _Generic((value),                                              \
        unsigned char:        stdc_bit_ceil_uc,                    \
        unsigned short:       stdc_bit_ceil_us,                    \
        unsigned int:         stdc_bit_ceil_ui,                    \
        unsigned long:        stdc_bit_ceil_ul,                    \
        unsigned long long:   stdc_bit_ceil_ull,                   \
        default:              stdc_bit_ceil_ui)((value))

#endif

#endif /* STDBIT_IMPL_H */