//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only

#include "./x86mem.h"
#include <stdint.h>
#include <stddef.h>

#ifndef x86memlib_DeclFunction
#define x86memlib_DeclFunction(name) name
#endif
#ifndef CACHESIZELIMIT
#define CACHESIZELIMIT (8u * 1024u * 1024u)
#endif

#ifdef __x86_64__

#ifdef __clang__
#define __m128i_u __m128i
#define __m256i_u __m256i
#define __m512i_u __m512i
#elif defined(__GNUC__) && __GNUC__ < 9
typedef __m128i __m128i_u;
typedef __m256i __m256i_u;
#ifdef __AVX512F__
typedef __m512i __m512i_u;
#endif
#endif

#undef BYTE_ALIGNMENT
#if defined(__AVX512F__)
#define BYTE_ALIGNMENT 0x3F
#elif defined(__AVX__)
#define BYTE_ALIGNMENT 0x1F
#else
#define BYTE_ALIGNMENT 0x0F
#endif

/* ==================== 存储策略宏 ==================== */

#define ST_S8(p,v)    (*(uint8_t  *)(p)=(v))
#define FIN_S8
#define ST_S16(p,v)   (*(uint16_t *)(p)=(v))
#define FIN_S16
#define ST_S32(p,v)   (*(uint32_t *)(p)=(v))
#define FIN_S32
#define ST_S64(p,v)   (*(uint64_t *)(p)=(v))
#define FIN_S64

#define ST_U128(p,v)  _mm_storeu_si128((__m128i *)(p),(v))
#define FIN_U128
#define ST_A128(p,v)  _mm_store_si128((__m128i *)(p),(v))
#define FIN_A128
/* SSE2 流式: _mm_stream_si128 是 SSE2, 不需要 #ifdef */
#define ST_T128(p,v)  _mm_stream_si128((__m128i *)(p),(v))
#define FIN_T128      _mm_sfence();

#ifdef __AVX__
#define ST_U256(p,v)  _mm256_storeu_si256((__m256i *)(p),(v))
#define FIN_U256
#define ST_A256(p,v)  _mm256_store_si256((__m256i *)(p),(v))
#define FIN_A256
#define ST_T256(p,v)  _mm256_stream_si256((__m256i *)(p),(v))
#define FIN_T256      _mm_sfence();
#endif

#ifdef __AVX512F__
#define ST_U512(p,v)  _mm512_storeu_si512((__m512i *)(p),(v))
#define FIN_U512
#define ST_A512(p,v)  _mm512_store_si512((__m512i *)(p),(v))
#define FIN_A512
#define ST_T512(p,v)  _mm512_stream_si512((__m512i *)(p),(v))
#define FIN_T512      _mm_sfence();
#endif

/* ==================== 步进宏 ==================== */

#define MS_STEP(P)     ST_##P(d, val); d++;

#define MS_STEPS_1(P)    MS_STEP(P)
#define MS_STEPS_2(P)    MS_STEP(P)  MS_STEP(P)
#define MS_STEPS_4(P)    MS_STEPS_2(P)  MS_STEPS_2(P)
#define MS_STEPS_8(P)    MS_STEPS_4(P)  MS_STEPS_4(P)
#define MS_STEPS_16(P)   MS_STEPS_8(P)  MS_STEPS_8(P)
#define MS_STEPS_32(P)   MS_STEPS_16(P) MS_STEPS_16(P)
#define MS_STEPS_64(P)   MS_STEPS_32(P) MS_STEPS_32(P)

/* ==================== 函数生成器 ==================== */

#define MS_DEF(NAME, VTYPE, POL, N)                                         \
static void *NAME(void *dest, const VTYPE val, size_t len)                   \
{                                                                            \
    VTYPE *d = (VTYPE *)dest;                                                \
    while (len--) {                                                          \
        MS_STEPS_##N(POL);                                                   \
    }                                                                        \
    FIN_##POL                                                               \
    return dest;                                                             \
}

/* ==================== 全部 55 个块函数 ==================== */

MS_DEF(memset_fpx86,         uint8_t,   S8,   1)
MS_DEF(memset_16bit,         uint16_t,  S16,  1)
MS_DEF(memset_32bit,         uint32_t,  S32,  1)
MS_DEF(memset_64bit,         uint64_t,  S64,  1)

MS_DEF(memset_128bit_u,      __m128i_u, U128, 1)
MS_DEF(memset_128bit_32B_u,  __m128i_u, U128, 2)
MS_DEF(memset_128bit_64B_u,  __m128i_u, U128, 4)
MS_DEF(memset_128bit_128B_u, __m128i_u, U128, 8)
MS_DEF(memset_128bit_256B_u, __m128i_u, U128, 16)

MS_DEF(memset_128bit_a,      __m128i,   A128, 1)
MS_DEF(memset_128bit_32B_a,  __m128i,   A128, 2)
MS_DEF(memset_128bit_64B_a,  __m128i,   A128, 4)
MS_DEF(memset_128bit_128B_a, __m128i,   A128, 8)
MS_DEF(memset_128bit_256B_a, __m128i,   A128, 16)

/* SSE2 流式: 无 #ifdef, _mm_stream_si128 是 SSE2 */
MS_DEF(memset_128bit_as,      __m128i,   T128, 1)
MS_DEF(memset_128bit_32B_as,  __m128i,   T128, 2)
MS_DEF(memset_128bit_64B_as,  __m128i,   T128, 4)
MS_DEF(memset_128bit_128B_as, __m128i,   T128, 8)
MS_DEF(memset_128bit_256B_as, __m128i,   T128, 16)

#ifdef __AVX__
MS_DEF(memset_256bit_u,      __m256i_u, U256, 1)
MS_DEF(memset_256bit_64B_u,  __m256i_u, U256, 2)
MS_DEF(memset_256bit_128B_u, __m256i_u, U256, 4)
MS_DEF(memset_256bit_256B_u, __m256i_u, U256, 8)
MS_DEF(memset_256bit_512B_u, __m256i_u, U256, 16)

MS_DEF(memset_256bit_a,      __m256i,   A256, 1)
MS_DEF(memset_256bit_64B_a,  __m256i,   A256, 2)
MS_DEF(memset_256bit_128B_a, __m256i,   A256, 4)
MS_DEF(memset_256bit_256B_a, __m256i,   A256, 8)
MS_DEF(memset_256bit_512B_a, __m256i,   A256, 16)

MS_DEF(memset_256bit_as,      __m256i,   T256, 1)
MS_DEF(memset_256bit_64B_as,  __m256i,   T256, 2)
MS_DEF(memset_256bit_128B_as, __m256i,   T256, 4)
MS_DEF(memset_256bit_256B_as, __m256i,   T256, 8)
MS_DEF(memset_256bit_512B_as, __m256i,   T256, 16)
#endif

#ifdef __AVX512F__
MS_DEF(memset_512bit_u,       __m512i_u, U512, 1)
MS_DEF(memset_512bit_128B_u,  __m512i_u, U512, 2)
MS_DEF(memset_512bit_256B_u,  __m512i_u, U512, 4)
MS_DEF(memset_512bit_512B_u,  __m512i_u, U512, 8)
MS_DEF(memset_512bit_1kB_u,   __m512i_u, U512, 16)
MS_DEF(memset_512bit_2kB_u,   __m512i_u, U512, 32)
MS_DEF(memset_512bit_4kB_u,   __m512i_u, U512, 64)

MS_DEF(memset_512bit_a,       __m512i,   A512, 1)
MS_DEF(memset_512bit_128B_a,  __m512i,   A512, 2)
MS_DEF(memset_512bit_256B_a,  __m512i,   A512, 4)
MS_DEF(memset_512bit_512B_a,  __m512i,   A512, 8)
MS_DEF(memset_512bit_1kB_a,   __m512i,   A512, 16)
MS_DEF(memset_512bit_2kB_a,   __m512i,   A512, 32)
MS_DEF(memset_512bit_4kB_a,   __m512i,   A512, 64)

MS_DEF(memset_512bit_as,      __m512i,   T512, 1)
MS_DEF(memset_512bit_128B_as, __m512i,   T512, 2)
MS_DEF(memset_512bit_256B_as, __m512i,   T512, 4)
MS_DEF(memset_512bit_512B_as, __m512i,   T512, 8)
MS_DEF(memset_512bit_1kB_as,  __m512i,   T512, 16)
MS_DEF(memset_512bit_2kB_as,  __m512i,   T512, 32)
MS_DEF(memset_512bit_4kB_as,  __m512i,   T512, 64)
#endif

/* ==================== 零填充函数 ==================== */

#if defined(__AVX512F__)
#define MS_ZERO_VAL  _mm512_setzero_si512()
typedef __m512i_u ms_zero_t;
#define MS_ZERO_LADDER \
    if      (numbytes < 16) { memset_fpx86(dest, 0, numbytes); numbytes = 0; } \
    else if (numbytes < 32)  MS_RUNG_ZERO(memset_512bit_u,      16) \
    else if (numbytes < 64)  MS_RUNG_ZERO(memset_512bit_u,       32) \
    else if (numbytes < 128) MS_RUNG_ZERO(memset_512bit_128B_u,  64) \
    else if (numbytes < 256) MS_RUNG_ZERO(memset_512bit_256B_u, 128) \
    else if (numbytes < 512) MS_RUNG_ZERO(memset_512bit_512B_u, 256) \
    else if (numbytes <1024) MS_RUNG_ZERO(memset_512bit_1kB_u,  512) \
    else if (numbytes <2048) MS_RUNG_ZERO(memset_512bit_2kB_u, 1024) \
    else                     MS_RUNG_ZERO(memset_512bit_4kB_u, 4096)

#define MS_ZERO_LADDER_AS \
    if      (numbytes < 16) { memset_fpx86(dest, 0, numbytes); numbytes = 0; } \
    else if (numbytes < 32)  MS_RUNG_ZERO_AS(memset_512bit_as,       16) \
    else if (numbytes < 64)  MS_RUNG_ZERO_AS(memset_512bit_as,       32) \
    else if (numbytes <128)  MS_RUNG_ZERO_AS(memset_512bit_128B_as,  64) \
    else if (numbytes <256)  MS_RUNG_ZERO_AS(memset_512bit_256B_as, 128) \
    else if (numbytes <512)  MS_RUNG_ZERO_AS(memset_512bit_512B_as, 256) \
    else if (numbytes<1024) MS_RUNG_ZERO_AS(memset_512bit_1kB_as,  512) \
    else if (numbytes<2048) MS_RUNG_ZERO_AS(memset_512bit_2kB_as, 1024) \
    else                     MS_RUNG_ZERO_AS(memset_512bit_4kB_as, 4096)

#elif defined(__AVX__)
#define MS_ZERO_VAL  _mm256_setzero_si256()
typedef __m256i_u ms_zero_t;
#define MS_ZERO_LADDER \
    if      (numbytes < 16) { memset_fpx86(dest, 0, numbytes); numbytes = 0; } \
    else if (numbytes < 32)  MS_RUNG_ZERO(memset_256bit_u,      16) \
    else if (numbytes < 64)  MS_RUNG_ZERO(memset_256bit_u,       32) \
    else if (numbytes <128)  MS_RUNG_ZERO(memset_256bit_64B_u,  64) \
    else if (numbytes <256)  MS_RUNG_ZERO(memset_256bit_128B_u, 128) \
    else if (numbytes <512)  MS_RUNG_ZERO(memset_256bit_256B_u, 256) \
    else                     MS_RUNG_ZERO(memset_256bit_512B_u, 512)

#define MS_ZERO_LADDER_AS \
    if      (numbytes < 16) { memset_fpx86(dest, 0, numbytes); numbytes = 0; } \
    else if (numbytes < 32)  MS_RUNG_ZERO_AS(memset_256bit_as,       16) \
    else if (numbytes < 64)  MS_RUNG_ZERO_AS(memset_256bit_as,       32) \
    else if (numbytes <128) MS_RUNG_ZERO_AS(memset_256bit_64B_as,   64) \
    else if (numbytes <256) MS_RUNG_ZERO_AS(memset_256bit_128B_as, 128) \
    else if (numbytes <512) MS_RUNG_ZERO_AS(memset_256bit_256B_as, 256) \
    else                     MS_RUNG_ZERO_AS(memset_256bit_512B_as, 512)

#else
#define MS_ZERO_VAL  _mm_setzero_si128()
typedef __m128i_u ms_zero_t;
#define MS_ZERO_LADDER \
    if      (numbytes < 16) { memset_fpx86(dest, 0, numbytes); numbytes = 0; } \
    else if (numbytes < 32)  MS_RUNG_ZERO(memset_128bit_u,      16) \
    else if (numbytes < 64)  MS_RUNG_ZERO(memset_128bit_32B_u,  32) \
    else if (numbytes <128) MS_RUNG_ZERO(memset_128bit_64B_u,  64) \
    else if (numbytes <256) MS_RUNG_ZERO(memset_128bit_128B_u, 128) \
    else                     MS_RUNG_ZERO(memset_128bit_256B_u, 256)

#define MS_ZERO_LADDER_AS \
    if      (numbytes < 16) { memset_fpx86(dest, 0, numbytes); numbytes = 0; } \
    else if (numbytes < 32)  MS_RUNG_ZERO_AS(memset_128bit_as,       16) \
    else if (numbytes < 64) MS_RUNG_ZERO_AS(memset_128bit_32B_as,    32) \
    else if (numbytes <128) MS_RUNG_ZERO_AS(memset_128bit_64B_as,   64) \
    else if (numbytes <256) MS_RUNG_ZERO_AS(memset_128bit_128B_as, 128) \
    else                     MS_RUNG_ZERO_AS(memset_128bit_256B_as, 256)
#endif

#define MS_RUNG_ZERO(FN, SZ)                      \
    {                                             \
        FN(dest, MS_ZERO_VAL, numbytes / (SZ));   \
        offset = numbytes & ~((size_t)(SZ) - 1);  \
        dest = (char *)dest + offset;             \
        numbytes &= (SZ) - 1;                     \
    }

#define MS_RUNG_ZERO_AS(FN, SZ)                   \
    {                                             \
        FN(dest, zval, numbytes / (SZ));          \
        offset = numbytes & ~((size_t)(SZ) - 1);  \
        dest = (char *)dest + offset;             \
        numbytes &= (SZ) - 1;                     \
    }

static void *memset_zeroes(void *dest, size_t numbytes) {
    void *ret = dest; size_t offset;
    while (numbytes) { MS_ZERO_LADDER }
    return ret;
}

static void *memset_zeroes_a(void *dest, size_t numbytes) {
    return memset_zeroes(dest, numbytes);
}

static void *memset_zeroes_as(void *dest, size_t numbytes) {
    void *ret = dest; size_t offset;
    ms_zero_t zval = MS_ZERO_VAL;
    while (numbytes) { MS_ZERO_LADDER_AS }
    return ret;
}

/* ==================== 阶梯分发器 ==================== */

#define MS_RUNG(FN, SET, SZ)                      \
    {                                             \
        FN(dest, SET, numbytes / (SZ));           \
        offset = numbytes & ~((size_t)(SZ) - 1);  \
        dest = (char *)dest + offset;             \
        numbytes &= (SZ) - 1;                     \
    }

#define MS_HEAD_RUNGS                                                     \
    if (numbytes < 16) { memset_fpx86(dest, val, numbytes); numbytes = 0; }

#if defined(__AVX512F__)
#define MS_SET1 _mm512_set1_epi8((char)val)
#define MS_LADDER_U \
    MS_HEAD_RUNGS                                                     \
    else if (numbytes < 32)   MS_RUNG(memset_512bit_u,       MS_SET1, 16) \
    else if (numbytes < 64)   MS_RUNG(memset_512bit_u,       MS_SET1, 32) \
    else if (numbytes < 128)  MS_RUNG(memset_512bit_128B_u,  MS_SET1, 64) \
    else if (numbytes < 256)  MS_RUNG(memset_512bit_256B_u, MS_SET1, 128) \
    else if (numbytes < 512)  MS_RUNG(memset_512bit_512B_u, MS_SET1, 256) \
    else if (numbytes <1024)  MS_RUNG(memset_512bit_1kB_u,  MS_SET1, 512) \
    else if (numbytes <2048)  MS_RUNG(memset_512bit_2kB_u,  MS_SET1,1024) \
    else                       MS_RUNG(memset_512bit_4kB_u,  MS_SET1,4096)
#define MS_LADDER_A MS_LADDER_U
#define MS_LADDER_AS \
    MS_HEAD_RUNGS                                                          \
    else if (numbytes < 32)   MS_RUNG(memset_512bit_as,       MS_SET1, 16) \
    else if (numbytes < 64)   MS_RUNG(memset_512bit_as,       MS_SET1, 32) \
    else if (numbytes < 128)  MS_RUNG(memset_512bit_128B_as,  MS_SET1, 64) \
    else if (numbytes < 256)  MS_RUNG(memset_512bit_256B_as, MS_SET1, 128) \
    else if (numbytes < 512)  MS_RUNG(memset_512bit_512B_as, MS_SET1, 256) \
    else if (numbytes <1024)  MS_RUNG(memset_512bit_1kB_as,  MS_SET1, 512) \
    else if (numbytes <2048)  MS_RUNG(memset_512bit_2kB_as,  MS_SET1,1024) \
    else                       MS_RUNG(memset_512bit_4kB_as,  MS_SET1,4096)

#elif defined(__AVX__)
#define MS_SET1 _mm256_set1_epi8((char)val)
#define MS_LADDER_U \
    MS_HEAD_RUNGS                                                     \
    else if (numbytes < 32)   MS_RUNG(memset_256bit_u,       MS_SET1, 16) \
    else if (numbytes < 64)   MS_RUNG(memset_256bit_u,       MS_SET1, 32) \
    else if (numbytes < 128)  MS_RUNG(memset_256bit_64B_u,   MS_SET1, 64) \
    else if (numbytes < 256)  MS_RUNG(memset_256bit_128B_u,  MS_SET1, 128) \
    else if (numbytes < 512)  MS_RUNG(memset_256bit_256B_u, MS_SET1, 256) \
    else                       MS_RUNG(memset_256bit_512B_u,  MS_SET1, 512)
#define MS_LADDER_A MS_LADDER_U
#define MS_LADDER_AS \
    MS_HEAD_RUNGS                                                          \
    else if (numbytes < 32)   MS_RUNG(memset_256bit_as,       MS_SET1, 16) \
    else if (numbytes < 64)   MS_RUNG(memset_256bit_as,       MS_SET1, 32) \
    else if (numbytes < 128)  MS_RUNG(memset_256bit_64B_as,   MS_SET1, 64) \
    else if (numbytes < 256)  MS_RUNG(memset_256bit_128B_as,  MS_SET1, 128) \
    else if (numbytes < 512)  MS_RUNG(memset_256bit_256B_as, MS_SET1, 256) \
    else                       MS_RUNG(memset_256bit_512B_as,  MS_SET1, 512)

#else
#define MS_SET1 _mm_set1_epi8((char)val)
#define MS_LADDER_U \
    MS_HEAD_RUNGS                                                     \
    else if (numbytes < 32)   MS_RUNG(memset_128bit_u,       MS_SET1, 16) \
    else if (numbytes < 64)   MS_RUNG(memset_128bit_32B_u,   MS_SET1, 32) \
    else if (numbytes < 128)  MS_RUNG(memset_128bit_64B_u,   MS_SET1, 64) \
    else if (numbytes < 256)  MS_RUNG(memset_128bit_128B_u, MS_SET1, 128) \
    else                       MS_RUNG(memset_128bit_256B_u,  MS_SET1, 256)
#define MS_LADDER_A MS_LADDER_U
#define MS_LADDER_AS \
    MS_HEAD_RUNGS                                                          \
    else if (numbytes < 32)   MS_RUNG(memset_128bit_as,       MS_SET1, 16) \
    else if (numbytes < 64)   MS_RUNG(memset_128bit_32B_as,   MS_SET1, 32) \
    else if (numbytes < 128)  MS_RUNG(memset_128bit_64B_as,   MS_SET1, 64) \
    else if (numbytes < 256)  MS_RUNG(memset_128bit_128B_as, MS_SET1, 128) \
    else                       MS_RUNG(memset_128bit_256B_as,  MS_SET1, 256)
#endif

#define MS_LADDER_FN(NAME, LADDER, ZERO_FN)                                 \
static void *NAME(void *dest, const uint8_t val, size_t numbytes)           \
{                                                                           \
    void *returnval = dest;                                                 \
    if (val == 0) { return ZERO_FN(dest, numbytes); }                      \
    size_t offset;                                                          \
    while (numbytes) { LADDER }                                            \
    return returnval;                                                       \
}

MS_LADDER_FN(memset_large,    MS_LADDER_U,  memset_zeroes)
MS_LADDER_FN(memset_large_a,  MS_LADDER_A,  memset_zeroes_a)
MS_LADDER_FN(memset_large_as, MS_LADDER_AS, memset_zeroes_as)

/* ==================== 主入口 ==================== */

void * x86memlib_DeclFunction(AVX_memset)(void *dest, const uint8_t val, size_t numbytes)
{
    if (numbytes == 0) return dest;

    if (val == 0) {
        if ((((uintptr_t)dest & BYTE_ALIGNMENT) == 0) && (numbytes > CACHESIZELIMIT))
            return memset_zeroes_as(dest, numbytes);
        return memset_zeroes(dest, numbytes);
    }

    if ((((uintptr_t)dest & BYTE_ALIGNMENT) == 0) && (numbytes > 16)) {
        if (numbytes > CACHESIZELIMIT) memset_large_as(dest, val, numbytes);
        else                           memset_large_a (dest, val, numbytes);
    } else {
        size_t head = (BYTE_ALIGNMENT + 1) - ((uintptr_t)dest & BYTE_ALIGNMENT);
        if ((uintptr_t)dest & BYTE_ALIGNMENT) {
            if (numbytes > head) {
                memset_fpx86(dest, val, head);
                dest = (char *)dest + head;
                numbytes -= head;
                if (numbytes > CACHESIZELIMIT) memset_large_as(dest, val, numbytes);
                else                           memset_large_a (dest, val, numbytes);
            } else {
                memset_fpx86(dest, val, numbytes);
            }
        } else {
            memset_large(dest, val, numbytes);
        }
    }

    return dest;
}

void * x86memlib_DeclFunction(AVX_memset_4B)(void *dest, const uint32_t val, size_t numbytes)
{
    size_t count = numbytes / 4;
    size_t rem   = numbytes & 3;

#if defined(__AVX512F__)
    __m512i v512 = _mm512_set1_epi32(val);
#endif
#if defined(__AVX__)
    __m256i v256 = _mm256_set1_epi32(val);
#endif
    __m128i v128 = _mm_set1_epi32(val);

#if defined(__AVX512F__)
    while (count >= 16) { _mm512_storeu_si512((__m512i*)dest, v512); dest = (char*)dest + 64; count -= 16; }
#endif
#if defined(__AVX__)
    while (count >= 8)  { _mm256_storeu_si256((__m256i*)dest, v256); dest = (char*)dest + 32; count -= 8; }
#endif
    while (count >= 4)  { _mm_storeu_si128((__m128i*)dest, v128); dest = (char*)dest + 16; count -= 4; }
    while (count--)     { *(uint32_t*)dest = val; dest = (char*)dest + 4; }
    if (rem)            { memset_fpx86(dest, (uint8_t)val, rem); }

    return dest;
}

#endif /* __x86_64__ */
#undef BYTE_ALIGNMENT

/* ==================== 多版本弱符号 ==================== */

#ifdef X86MEM_NOT_COMPILE_AVX512
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 0;
__attribute__((weak, used)) void *AVX_memsetV3(void *dest, const uint8_t val, size_t n) {return dest;}
__attribute__((weak, used)) void *AVX_memset_4BV3(void *dest, const uint32_t val, size_t n) {return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX2
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 0;
__attribute__((weak, used)) void *AVX_memsetV2(void *dest, const uint8_t val, size_t n) {return dest;}
__attribute__((weak, used)) void *AVX_memset_4BV2(void *dest, const uint32_t val, size_t n) {return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 0;
__attribute__((weak, used)) void *AVX_memsetV1(void *dest, const uint8_t val, size_t n) {return dest;}
__attribute__((weak, used)) void *AVX_memset_4BV1(void *dest, const uint32_t val, size_t n) {return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 1;
#endif