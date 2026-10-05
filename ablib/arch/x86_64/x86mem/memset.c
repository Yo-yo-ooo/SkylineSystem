//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* ============================================================================
 * x86mem :: memset —— 分层实现 (SSE4.2 / AVX / AVX2 / AVX512F)
 *
 * 修复的旧版缺陷 (均为实测可触发):
 *  [B1] 阶梯 rung 的 SZ 与块函数实际块宽不匹配, 导致越界写入。
 *       例 (AVX2 tier): 16<=n<32 时调用 memset_256bit_u(len=n/16=1),
 *       该块函数每次迭代写 32 字节 —— 最多越界 16B。
 *       AVX512 tier 更严重: 1kB<=n<2kB 时调用 memset_512bit_1kB_u(len=1)
 *       一次写 1024 字节, 最多越界 512B; 2kB 档最多越界 1024B。
 *  [B2] AVX_memset 在"先对齐头部再填充主体"的分支里返回的是已推进过的
 *       dest 指针, 违反 memset 必须返回原始入参的契约。
 *  [B3] n<16 一律退化成逐字节 memset_fpx86, 小尺寸性能极差。
 *
 * 新实现: 小块首尾夹逼 (零越界) + 对齐后 4x 向量主循环 + 末尾重叠向量收尾,
 * 超大尺寸走 NT 存储。返回值恒为原始 dest。
 * ==========================================================================*/
#include "./x86mem.h"
#include <stdint.h>
#include <stddef.h>
#include <emmintrin.h>

#ifdef __x86_64__

/* 注意: 阈值不再是编译期常量 —— x86mem_cache_limit 由启动代码按 L3 容量填 */



typedef uint16_t __attribute__((__may_alias__, aligned(1))) xm_u16;
typedef uint32_t __attribute__((__may_alias__, aligned(1))) xm_u32;
typedef uint64_t __attribute__((__may_alias__, aligned(1))) xm_u64;

/* ---------------------------- 向量宽度选择 ---------------------------- */
#if defined(__AVX512F__)
typedef __m512i   xm_vec;
#define XM_VEC         64u
#define XM_STOREU(p,v) _mm512_storeu_si512((void *)(p), (v))
#define XM_STOREA(p,v) _mm512_store_si512((void *)(p), (v))
#define XM_STORENT(p,v) _mm512_stream_si512((__m512i *)(void *)(p), (v))
#define XM_SET1(c)    _mm512_set1_epi8((char)(c))
#define XM_SETZERO()  _mm512_setzero_si512()
#elif defined(__AVX__)
typedef __m256i   xm_vec;
#define XM_VEC         32u
#define XM_STOREU(p,v) _mm256_storeu_si256((__m256i_u *)(void *)(p), (v))
#define XM_STOREA(p,v) _mm256_store_si256((__m256i *)(void *)(p), (v))
#define XM_STORENT(p,v) _mm256_stream_si256((__m256i *)(void *)(p), (v))
#define XM_SET1(c)    _mm256_set1_epi8((char)(c))
#define XM_SETZERO()  _mm256_setzero_si256()
#else
typedef __m128i   xm_vec;
#define XM_VEC         16u
#define XM_STOREU(p,v) _mm_storeu_si128((__m128i_u *)(void *)(p), (v))
#define XM_STOREA(p,v) _mm_store_si128((__m128i *)(void *)(p), (v))
#define XM_STORENT(p,v) _mm_stream_si128((__m128i *)(void *)(p), (v))
#define XM_SET1(c)    _mm_set1_epi8((char)(c))
#define XM_SETZERO()  _mm_setzero_si128()
#endif

#define XM_ST16(p,v)  (*(xm_u16 *)(p) = (v))
#define XM_ST32(p,v)  (*(xm_u32 *)(p) = (v))
#define XM_ST64(p,v)  (*(xm_u64 *)(p) = (v))

#define XM_V128_STOREU(p,v) _mm_storeu_si128((__m128i_u *)(void *)(p), (v))

/* 主循环展开块 (4 次连续存储)。
 * 展开度实测敏感: 4x 在目标机器 (Alder Lake) 上最稳, 8x 反而掉 ~15%。
 * 改这个值必须重跑 tests/memops 基准。 */
#define XM_BLK(STORE)                                                      \
    do {                                                                   \
        STORE(d,               v); STORE(d + XM_VEC,      v);              \
        STORE(d + 2u * XM_VEC, v); STORE(d + 3u * XM_VEC, v);              \
    } while (0)

/* ============================================================================
 * 小块填充: n < 64, 首尾夹逼, 写入严格落在 [d, d+n)
 * ==========================================================================*/
static inline void xm_fill_small(uint8_t *d, uint8_t val, size_t n)
{
    if (n == 0) return;

    if (n <= 15) {
        if (n < 8) {
            if (n < 4) {
                if (n < 2) { d[0] = val; return; }
                uint16_t w = (uint16_t)val * 0x0101u;
                XM_ST16(d, w);
                XM_ST16(d + n - 2, w);
                return;
            }
            uint32_t q = (uint32_t)val * 0x01010101u;
            XM_ST32(d, q);
            XM_ST32(d + n - 4, q);
            return;
        }
        uint64_t o = (uint64_t)val * 0x0101010101010101ull;
        XM_ST64(d, o);
        XM_ST64(d + n - 8, o);
        return;
    }

    /* 16 <= n < 64: 16 字节块首尾夹逼 */
    __m128i v16 = _mm_set1_epi8((char)val);
    XM_V128_STOREU(d, v16);
    XM_V128_STOREU(d + n - 16, v16);
    if (n > 32) {
        XM_V128_STOREU(d + 16, v16);
        XM_V128_STOREU(d + n - 32, v16);
    }
}

/* ============================================================================
 * 主填充循环: 前置条件 n >= XM_VEC
 * ==========================================================================*/
static void xm_fill(uint8_t *d, xm_vec v, size_t n, int nt)
{
    size_t mis = (size_t)((uintptr_t)d & (XM_VEC - 1u));

    if (mis) {
        size_t head = XM_VEC - mis;      /* 1 .. XM_VEC-1, 恒 < n */
        XM_STOREU(d, v);
        d += head;
        n -= head;
    }
    /* 此后 d 恒按 XM_VEC 对齐 */

    if (nt) {
        while (n >= 4u * XM_VEC) {
            XM_BLK(XM_STORENT);
            d += 4u * XM_VEC;
            n -= 4u * XM_VEC;
        }
    } else {
        while (n >= 4u * XM_VEC) {          /* d 已对齐: 用对齐存储 */
            XM_BLK(XM_STOREA);
            d += 4u * XM_VEC;
            n -= 4u * XM_VEC;
        }
    }

    while (n >= XM_VEC) {
        XM_STOREA(d, v);
        d += XM_VEC;
        n -= XM_VEC;
    }

    /* 剩余 1 .. XM_VEC-1 字节: 写入窗口 [end-XM_VEC, end) 恒在界内 */
    if (n) XM_STOREU(d + n - XM_VEC, v);

    if (nt) _mm_sfence();
}

/* ============================================================================
 * 主入口 (返回值恒为原始 dest —— 旧版在对齐分支里返回了推进后的指针)
 * ==========================================================================*/
void * x86memlib_DeclFunction(AVX_memset)(void *dest, const uint8_t val, size_t numbytes)
{
    uint8_t *d = (uint8_t *)dest;

    if (numbytes == 0) return dest;

    if (numbytes < 64u) {
        xm_fill_small(d, val, numbytes);
        return dest;
    }

    /* val==0 走 setzero: 省掉一次 GPR->向量广播 (vpbroadcastb 需 AVX2/512BW) */
    xm_vec v = (val == 0) ? XM_SETZERO() : XM_SET1(val);

    xm_fill(d, v, numbytes, numbytes > x86mem_cache_limit);
    return dest;
}

/* 4 字节粒度填充 (旧接口保留, 语义: 以 val 为单位填充 numbytes 字节) */
void * x86memlib_DeclFunction(AVX_memset_4B)(void *dest, const uint32_t val, size_t numbytes)
{
    uint8_t *d = (uint8_t *)dest;
    size_t   n = numbytes & ~(size_t)3u;
    size_t   rem = numbytes & (size_t)3u;
    __m128i  v128 = _mm_set1_epi32((int)val);
#if defined(__AVX__)
    __m256i  v256 = _mm256_set1_epi32((int)val);
#endif
#if defined(__AVX512F__)
    __m512i  v512 = _mm512_set1_epi32((int)val);
#endif

    if (n == 0) {
        uint32_t tail = val;
        const uint8_t *tb = (const uint8_t *)&tail;
        for (size_t i = 0; i < rem; i++) d[i] = tb[i];
        return dest;
    }

#if defined(__AVX512F__)
    while (n >= 64u) { _mm512_storeu_si512((void *)d, v512); d += 64u; n -= 64u; }
#endif
#if defined(__AVX__)
    while (n >= 32u) { _mm256_storeu_si256((__m256i_u *)(void *)d, v256); d += 32u; n -= 32u; }
#endif
    while (n >= 16u) { _mm_storeu_si128((__m128i_u *)(void *)d, v128); d += 16u; n -= 16u; }
    while (n >= 4u)  { XM_ST32(d, (uint32_t)val); d += 4u; n -= 4u; }

    if (rem) {
        uint32_t tail = val;
        const uint8_t *tb = (const uint8_t *)&tail;
        for (size_t i = 0; i < rem; i++) d[i] = tb[i];
    }
    return dest;
}

#endif /* __x86_64__ */

/* ============================================================================
 * 多版本弱符号
 * ==========================================================================*/

#ifdef X86MEM_NOT_COMPILE_AVX512
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 0;
__attribute__((weak, used)) void *AVX_memsetV3(void *dest, const uint8_t val, size_t n) {(void)val;(void)n;return dest;}
__attribute__((weak, used)) void *AVX_memset_4BV3(void *dest, const uint32_t val, size_t n) {(void)val;(void)n;return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX2
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 0;
__attribute__((weak, used)) void *AVX_memsetV2(void *dest, const uint8_t val, size_t n) {(void)val;(void)n;return dest;}
__attribute__((weak, used)) void *AVX_memset_4BV2(void *dest, const uint32_t val, size_t n) {(void)val;(void)n;return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 0;
__attribute__((weak, used)) void *AVX_memsetV1(void *dest, const uint8_t val, size_t n) {(void)val;(void)n;return dest;}
__attribute__((weak, used)) void *AVX_memset_4BV1(void *dest, const uint32_t val, size_t n) {(void)val;(void)n;return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 1;
#endif
