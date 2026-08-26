//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include "./x86mem.h"
#include <stdint.h>
#include <stddef.h>
#include <emmintrin.h>
//#include <immintrin.h>

#ifndef x86memlib_DeclFunction
#define x86memlib_DeclFunction(name) name
#endif
#ifndef x86memlib_UseFunction
#define x86memlib_UseFunction(name) name
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

/* ==================== 装载/存储策略宏 ====================
 *  命名: S=标量 U=非对齐 A=对齐 T=流式(NT)
 *  所有 intrinsic 与原版逐一对应, 不替换 lddqu→loadu 等 */

#define LD_S8(p)      (*(const uint8_t  *)(p))
#define ST_S8(p,v)    (*(uint8_t  *)(p)=(v))
#define FIN_S8

#define LD_S16(p)     (*(const uint16_t *)(p))
#define ST_S16(p,v)   (*(uint16_t *)(p)=(v))
#define FIN_S16

#define LD_S32(p)     (*(const uint32_t *)(p))
#define ST_S32(p,v)   (*(uint32_t *)(p)=(v))
#define FIN_S32

#define LD_S64(p)     (*(const uint64_t *)(p))
#define ST_S64(p,v)   (*(uint64_t *)(p)=(v))
#define FIN_S64

#define LD_U128(p)    _mm_lddqu_si128((const __m128i *)(p))
#define ST_U128(p,v)  _mm_storeu_si128((__m128i *)(p),(v))
#define FIN_U128

#define LD_A128(p)    _mm_load_si128((const __m128i *)(p))
#define ST_A128(p,v)  _mm_store_si128((__m128i *)(p),(v))
#define FIN_A128

#define LD_T128(p)    _mm_stream_load_si128((__m128i *)(p))
#define ST_T128(p,v)  _mm_stream_si128((__m128i *)(p),(v))
#define FIN_T128      _mm_sfence();

#ifdef __AVX__
#define LD_U256(p)    _mm256_lddqu_si256((const __m256i *)(p))
#define ST_U256(p,v)  _mm256_storeu_si256((__m256i *)(p),(v))
#define FIN_U256

#define LD_A256(p)    _mm256_load_si256((const __m256i *)(p))
#define ST_A256(p,v)  _mm256_store_si256((__m256i *)(p),(v))
#define FIN_A256
#endif

#ifdef __AVX2__
#define LD_T256(p)    _mm256_stream_load_si256((const __m256i *)(p))
#define ST_T256(p,v)  _mm256_stream_si256((__m256i *)(p),(v))
#define FIN_T256      _mm_sfence();
#endif

#ifdef __AVX512F__
#define LD_U512(p)    _mm512_loadu_si512((const __m512i *)(p))
#define ST_U512(p,v)  _mm512_storeu_si512((__m512i *)(p),(v))
#define FIN_U512

#define LD_A512(p)    _mm512_load_si512((const __m512i *)(p))
#define ST_A512(p,v)  _mm512_store_si512((__m512i *)(p),(v))
#define FIN_A512

#define LD_T512(p)    _mm512_stream_load_si512((const __m512i *)(p))
#define ST_T512(p,v)  _mm512_stream_si512((__m512i *)(p),(v))
#define FIN_T512      _mm_sfence();
#endif

#define XM_STEP(P)     ST_##P(d, LD_##P(s)); d++; s++;

#define XM_STEPS_1(P)    XM_STEP(P)
#define XM_STEPS_2(P)    XM_STEP(P)  XM_STEP(P)
#define XM_STEPS_4(P)    XM_STEPS_2(P)  XM_STEPS_2(P)
#define XM_STEPS_8(P)    XM_STEPS_4(P)  XM_STEPS_4(P)
#define XM_STEPS_16(P)   XM_STEPS_8(P)  XM_STEPS_8(P)
#define XM_STEPS_32(P)   XM_STEPS_16(P) XM_STEPS_16(P)
#define XM_STEPS_64(P)   XM_STEPS_32(P) XM_STEPS_32(P)

#define XM_DEF(NAME, VTYPE, POL, N)                                          \
static void *NAME(void *dest, const void *src, size_t len)                    \
{                                                                             \
    VTYPE       *d = (VTYPE *)dest;                                           \
    const VTYPE *s = (const VTYPE *)src;                                      \
    while (len--) {                                                           \
        XM_STEPS_##N(POL);                                                    \
    }                                                                         \
    FIN_##POL                                                                 \
    return dest;                                                              \
}

/* ==================== 全部 55 个块函数 ==================== */

/* ---- 标量 ---- */
XM_DEF(memcpy_fpx86,          uint8_t,   S8,   1)
XM_DEF(memcpy_16bit,          uint16_t,  S16,  1)
XM_DEF(memcpy_32bit,          uint32_t,  S32,  1)
XM_DEF(memcpy_64bit,          uint64_t,  S64,  1)

/* ---- SSE2 非对齐 / 对齐 / 流式 ---- */
XM_DEF(memcpy_128bit_u,       __m128i_u, U128, 1)
XM_DEF(memcpy_128bit_32B_u,   __m128i_u, U128, 2)
XM_DEF(memcpy_128bit_64B_u,   __m128i_u, U128, 4)
XM_DEF(memcpy_128bit_128B_u,  __m128i_u, U128, 8)
XM_DEF(memcpy_128bit_256B_u,  __m128i_u, U128, 16)

XM_DEF(memcpy_128bit_a,       __m128i,   A128, 1)
XM_DEF(memcpy_128bit_32B_a,   __m128i,   A128, 2)
XM_DEF(memcpy_128bit_64B_a,   __m128i,   A128, 4)
XM_DEF(memcpy_128bit_128B_a,  __m128i,   A128, 8)
XM_DEF(memcpy_128bit_256B_a,  __m128i,   A128, 16)

XM_DEF(memcpy_128bit_as,      __m128i,   T128, 1)
XM_DEF(memcpy_128bit_32B_as,  __m128i,   T128, 2)
XM_DEF(memcpy_128bit_64B_as,  __m128i,   T128, 4)
XM_DEF(memcpy_128bit_128B_as, __m128i,   T128, 8)
XM_DEF(memcpy_128bit_256B_as, __m128i,   T128, 16)

#ifdef __AVX__
XM_DEF(memcpy_256bit_u,       __m256i_u, U256, 1)
XM_DEF(memcpy_256bit_64B_u,   __m256i_u, U256, 2)
XM_DEF(memcpy_256bit_128B_u,  __m256i_u, U256, 4)
XM_DEF(memcpy_256bit_256B_u,  __m256i_u, U256, 8)
XM_DEF(memcpy_256bit_512B_u,  __m256i_u, U256, 16)

XM_DEF(memcpy_256bit_a,       __m256i,   A256, 1)
XM_DEF(memcpy_256bit_64B_a,   __m256i,   A256, 2)
XM_DEF(memcpy_256bit_128B_a,  __m256i,   A256, 4)
XM_DEF(memcpy_256bit_256B_a,  __m256i,   A256, 8)
XM_DEF(memcpy_256bit_512B_a,  __m256i,   A256, 16)
#endif

#ifdef __AVX2__
XM_DEF(memcpy_256bit_as,      __m256i,   T256, 1)
XM_DEF(memcpy_256bit_64B_as,  __m256i,   T256, 2)
XM_DEF(memcpy_256bit_128B_as, __m256i,   T256, 4)
XM_DEF(memcpy_256bit_256B_as, __m256i,   T256, 8)
XM_DEF(memcpy_256bit_512B_as, __m256i,   T256, 16)
#endif

#ifdef __AVX512F__
XM_DEF(memcpy_512bit_u,        __m512i_u, U512, 1)
XM_DEF(memcpy_512bit_128B_u,  __m512i_u, U512, 2)
XM_DEF(memcpy_512bit_256B_u,  __m512i_u, U512, 4)
XM_DEF(memcpy_512bit_512B_u,  __m512i_u, U512, 8)
XM_DEF(memcpy_512bit_1kB_u,   __m512i_u, U512, 16)
XM_DEF(memcpy_512bit_2kB_u,   __m512i_u, U512, 32)
XM_DEF(memcpy_512bit_4kB_u,   __m512i_u, U512, 64)

XM_DEF(memcpy_512bit_a,        __m512i,   A512, 1)
XM_DEF(memcpy_512bit_128B_a,  __m512i,   A512, 2)
XM_DEF(memcpy_512bit_256B_a,  __m512i,   A512, 4)
XM_DEF(memcpy_512bit_512B_a,  __m512i,   A512, 8)
XM_DEF(memcpy_512bit_1kB_a,   __m512i,   A512, 16)
XM_DEF(memcpy_512bit_2kB_a,   __m512i,   A512, 32)
XM_DEF(memcpy_512bit_4kB_a,   __m512i,   A512, 64)

XM_DEF(memcpy_512bit_as,       __m512i,   T512, 1)
XM_DEF(memcpy_512bit_128B_as, __m512i,   T512, 2)
XM_DEF(memcpy_512bit_256B_as, __m512i,   T512, 4)
XM_DEF(memcpy_512bit_512B_as, __m512i,   T512, 8)
XM_DEF(memcpy_512bit_1kB_as,  __m512i,   T512, 16)
XM_DEF(memcpy_512bit_2kB_as,  __m512i,   T512, 32)
XM_DEF(memcpy_512bit_4kB_as,  __m512i,   T512, 64)
#endif



#define XM_RUNG(FN, SZ)                            \
    {                                              \
        FN(dest, src, numbytes / (SZ));            \
        offset = numbytes & ~((size_t)(SZ) - 1);   \
        dest = (char *)dest + offset;               \
        src  = (char *)src  + offset;               \
        numbytes &= (SZ) - 1;                      \
    }

#define XM_HEAD_RUNGS                                                    \
    if      (numbytes < 2)  XM_RUNG(memcpy_fpx86,  1)                   \
    else if (numbytes < 4)  XM_RUNG(memcpy_16bit,  2)                   \
    else if (numbytes < 8)  XM_RUNG(memcpy_32bit,  4)                   \
    else if (numbytes < 16) XM_RUNG(memcpy_64bit,  8)

#define XM_LADDER_U128                                                  \
    XM_HEAD_RUNGS                                                       \
    else if (numbytes < 32)   XM_RUNG(memcpy_128bit_u,      16)         \
    else if (numbytes < 64)   XM_RUNG(memcpy_128bit_32B_u,  32)        \
    else if (numbytes < 128)  XM_RUNG(memcpy_128bit_64B_u,  64)        \
    else if (numbytes < 256)  XM_RUNG(memcpy_128bit_128B_u, 128)       \
    else                      XM_RUNG(memcpy_128bit_256B_u, 256)

#define XM_LADDER_A128                                                  \
    XM_HEAD_RUNGS                                                       \
    else if (numbytes < 32)   XM_RUNG(memcpy_128bit_a,      16)         \
    else if (numbytes < 64)   XM_RUNG(memcpy_128bit_32B_a,  32)         \
    else if (numbytes < 128)  XM_RUNG(memcpy_128bit_64B_a,  64)         \
    else if (numbytes < 256)  XM_RUNG(memcpy_128bit_128B_a, 128)        \
    else                      XM_RUNG(memcpy_128bit_256B_a, 256)

#define XM_LADDER_T128                                                  \
    XM_HEAD_RUNGS                                                       \
    else if (numbytes < 32)   XM_RUNG(memcpy_128bit_as,      16)        \
    else if (numbytes < 64)   XM_RUNG(memcpy_128bit_32B_as,  32)        \
    else if (numbytes < 128)  XM_RUNG(memcpy_128bit_64B_as,  64)        \
    else if (numbytes < 256)  XM_RUNG(memcpy_128bit_128B_as, 128)       \
    else                      XM_RUNG(memcpy_128bit_256B_as, 256)

#ifdef __AVX__
#define XM_LADDER_U256                                                  \
    XM_HEAD_RUNGS                                                       \
    else if (numbytes < 32)   XM_RUNG(memcpy_128bit_u,      16)        \
    else if (numbytes < 64)   XM_RUNG(memcpy_256bit_u,      32)        \
    else if (numbytes < 128)  XM_RUNG(memcpy_256bit_64B_u,  64)        \
    else if (numbytes < 256)  XM_RUNG(memcpy_256bit_128B_u, 128)       \
    else if (numbytes < 512)  XM_RUNG(memcpy_256bit_256B_u, 256)       \
    else                      XM_RUNG(memcpy_256bit_512B_u, 512)

#define XM_LADDER_A256                                                  \
    XM_HEAD_RUNGS                                                       \
    else if (numbytes < 32)   XM_RUNG(memcpy_128bit_a,      16)        \
    else if (numbytes < 64)   XM_RUNG(memcpy_256bit_a,      32)        \
    else if (numbytes < 128)  XM_RUNG(memcpy_256bit_64B_a,  64)        \
    else if (numbytes < 256)  XM_RUNG(memcpy_256bit_128B_a, 128)       \
    else if (numbytes < 512)  XM_RUNG(memcpy_256bit_256B_a, 256)       \
    else                      XM_RUNG(memcpy_256bit_512B_a, 512)
#endif

#ifdef __AVX2__
#define XM_LADDER_T256                                                  \
    XM_HEAD_RUNGS                                                       \
    else if (numbytes < 32)   XM_RUNG(memcpy_128bit_as,      16)       \
    else if (numbytes < 64)   XM_RUNG(memcpy_256bit_as,      32)       \
    else if (numbytes < 128)  XM_RUNG(memcpy_256bit_64B_as,  64)       \
    else if (numbytes < 256)  XM_RUNG(memcpy_256bit_128B_as, 128)       \
    else if (numbytes < 512)  XM_RUNG(memcpy_256bit_256B_as, 256)       \
    else                      XM_RUNG(memcpy_256bit_512B_as, 512)
#endif

#ifdef __AVX512F__
#define XM_LADDER_U512                                                  \
    XM_HEAD_RUNGS                                                       \
    else if (numbytes < 32)    XM_RUNG(memcpy_128bit_u,       16)       \
    else if (numbytes < 64)    XM_RUNG(memcpy_256bit_u,       32)       \
    else if (numbytes < 128)   XM_RUNG(memcpy_512bit_u,       64)       \
    else if (numbytes < 256)   XM_RUNG(memcpy_512bit_128B_u,  128)      \
    else if (numbytes < 512)   XM_RUNG(memcpy_512bit_256B_u,  256)      \
    else if (numbytes < 1024)  XM_RUNG(memcpy_512bit_512B_u,  512)      \
    else if (numbytes < 2048)  XM_RUNG(memcpy_512bit_1kB_u,   1024)     \
    else if (numbytes < 4096)  XM_RUNG(memcpy_512bit_2kB_u,   2048)     \
    else                       XM_RUNG(memcpy_512bit_4kB_u,   4096)

#define XM_LADDER_A512                                                  \
    XM_HEAD_RUNGS                                                       \
    else if (numbytes < 32)    XM_RUNG(memcpy_128bit_a,       16)       \
    else if (numbytes < 64)    XM_RUNG(memcpy_256bit_a,       32)       \
    else if (numbytes < 128)   XM_RUNG(memcpy_512bit_a,       64)       \
    else if (numbytes < 256)   XM_RUNG(memcpy_512bit_128B_a,  128)      \
    else if (numbytes < 512)   XM_RUNG(memcpy_512bit_256B_a,  256)      \
    else if (numbytes < 1024)  XM_RUNG(memcpy_512bit_512B_a,  512)      \
    else if (numbytes < 2048)  XM_RUNG(memcpy_512bit_1kB_a,   1024)     \
    else if (numbytes < 4096)  XM_RUNG(memcpy_512bit_2kB_a,   2048)     \
    else                       XM_RUNG(memcpy_512bit_4kB_a,   4096)

#define XM_LADDER_T512                                                  \
    XM_HEAD_RUNGS                                                       \
    else if (numbytes < 32)    XM_RUNG(memcpy_128bit_as,       16)     \
    else if (numbytes < 64)    XM_RUNG(memcpy_256bit_as,       32)      \
    else if (numbytes < 128)   XM_RUNG(memcpy_512bit_as,       64)      \
    else if (numbytes < 256)   XM_RUNG(memcpy_512bit_128B_as,  128)     \
    else if (numbytes < 512)   XM_RUNG(memcpy_512bit_256B_as,  256)     \
    else if (numbytes < 1024)  XM_RUNG(memcpy_512bit_512B_as,  512)     \
    else if (numbytes < 2048)  XM_RUNG(memcpy_512bit_1kB_as,   1024)    \
    else if (numbytes < 4096)  XM_RUNG(memcpy_512bit_2kB_as,   2048)    \
    else                       XM_RUNG(memcpy_512bit_4kB_as,   4096)
#endif

#define XM_LADDER_FN(NAME, LADDER)                                         \
static void *NAME(void *dest, void *src, size_t numbytes)                  \
{                                                                          \
    void *returnval = dest;                                                \
    size_t offset;                                                         \
    while (numbytes) { LADDER }                                            \
    return returnval;                                                      \
}

#if defined(__AVX512F__)
XM_LADDER_FN(memcpy_large,    XM_LADDER_U512)
XM_LADDER_FN(memcpy_large_a,  XM_LADDER_A512)
XM_LADDER_FN(memcpy_large_as, XM_LADDER_T512)
#elif defined(__AVX__)
XM_LADDER_FN(memcpy_large,    XM_LADDER_U256)
XM_LADDER_FN(memcpy_large_a,  XM_LADDER_A256)
#ifdef __AVX2__
XM_LADDER_FN(memcpy_large_as, XM_LADDER_T256)
#else
XM_LADDER_FN(memcpy_large_as, XM_LADDER_T128)
#endif
#else
XM_LADDER_FN(memcpy_large,    XM_LADDER_U128)
XM_LADDER_FN(memcpy_large_a,  XM_LADDER_A128)
XM_LADDER_FN(memcpy_large_as, XM_LADDER_T128)
#endif

/* ==================== 主入口 ==================== */

#ifdef OVERLAP_CHECK
void *x86memlib_UseFunction(AVX_memmove)(void *dest, void *src, size_t numbytes);
#endif

void * x86memlib_DeclFunction(AVX_memcpy)(void *dest, void *src, size_t numbytes)
{
    void *returnval = dest;

    if ((char *)src == (char *)dest) return returnval;

#ifdef OVERLAP_CHECK
    if (((char *)dest > (char *)src && (char *)dest < (char *)src + numbytes) ||
        ((char *)src  > (char *)dest && (char *)src  < (char *)dest + numbytes)) {
        return x86memlib_UseFunction(AVX_memmove)(dest, src, numbytes);
    }
#endif

    if ((((uintptr_t)src | (uintptr_t)dest) & BYTE_ALIGNMENT) == 0) {
        if (numbytes > CACHESIZELIMIT) memcpy_large_as(dest, src, numbytes);
        else                           memcpy_large_a (dest, src, numbytes);
    } else {
        size_t head = (BYTE_ALIGNMENT + 1) - ((uintptr_t)dest & BYTE_ALIGNMENT);
        if ((uintptr_t)dest & BYTE_ALIGNMENT) {
            if (numbytes > head) {
                memcpy_large(dest, src, head);
                dest = (char *)dest + head;
                src  = (char *)src  + head;
                numbytes -= head;
            } else {
                memcpy_large(dest, src, numbytes);
                return returnval;
            }
        }
        if (numbytes > CACHESIZELIMIT)
            memcpy_large_as(dest, src, numbytes);
        else if (((uintptr_t)src & BYTE_ALIGNMENT) == 0)
            memcpy_large_a(dest, src, numbytes);
        else
            memcpy_large(dest, src, numbytes);
    }

    return returnval;
}

#endif /* __x86_64__ */

#undef BYTE_ALIGNMENT

/* ==================== 多版本弱符号 ==================== */

#ifdef X86MEM_NOT_COMPILE_AVX512
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 0;
__attribute__((weak, used)) void *AVX_memcpyV3(void *dest, void *src, size_t n) {return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX2
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 0;
__attribute__((weak, used)) void *AVX_memcpyV2(void *dest, void *src, size_t n) {return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 0;
__attribute__((weak, used)) void *AVX_memcpyV1(void *dest, void *src, size_t n) {return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 1;
#endif