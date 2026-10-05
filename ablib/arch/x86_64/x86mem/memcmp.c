//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* ============================================================================
 * x86mem :: memcmp —— 分层实现 (SSE4.2 / AVX / AVX2 / AVX512F)
 *
 * 修复的旧版缺陷:
 *  [B1] 语义错误 —— 旧版按 16/32/64 位"整单元"做无符号大小比较来决定正负号。
 *      小端下这与 memcmp 的"首个差异字节"语义不等价。例:
 *        a = 01 00 00 00 00 00 00 02   (u64 = 0x0200000000000001)
 *        b = 02 00 00 00 00 00 00 01   (u64 = 0x0100000000000002)
 *      标准 memcmp 应返回负 (a[0]=01 < b[0]=02), 旧版返回正。
 *      新实现一律先定位"首个差异字节"再比字节, 与 C 标准完全一致。
 *  [B2] 无谓的 _a (对齐) 变体在完全未对齐的输入上被调用, 依赖调用方保证
 *      对齐却没有任何断言; 新实现全程只用非对齐加载。
 *
 * 分派: equality==0 → 相等性快速路径 (0 相等 / -1 不等);
 *       equality!=0 → 完整三态比较。
 * ==========================================================================*/
#include "./x86mem.h"
#include <stdint.h>
#include <stddef.h>
#include <emmintrin.h>

#ifdef __x86_64__

typedef uint16_t __attribute__((__may_alias__, aligned(1))) xm_u16;
typedef uint32_t __attribute__((__may_alias__, aligned(1))) xm_u32;
typedef uint64_t __attribute__((__may_alias__, aligned(1))) xm_u64;

#define XM_LD16(p) (*(const xm_u16 *)(p))
#define XM_LD32(p) (*(const xm_u32 *)(p))
#define XM_LD64(p) (*(const xm_u64 *)(p))

/* --------------------- 比较向量宽度 (与拷贝宽度解耦) ---------------------
 * AVX512F 无 AVX512BW, 拿不到 _mm512_cmpeq_epi8_mask; 改用 AVX512F 就有的
 * _mm512_cmpeq_epi64_mask (8 个 64 位 lane), 定位到 lane 后再精确定位字节。
 * AVX2 / SSE 直接用 movemask_epi8 拿到逐字节掩码。
 * --------------------------------------------------------------------- */
#if defined(__AVX512F__)
typedef __m512i  xm_cv;
#define XM_CV         64u
#define XM_CV_LOADU(p) _mm512_loadu_si512((const void *)(p))
#define XM_CV_EQ(a,b) ((unsigned)_mm512_cmpeq_epi64_mask((a), (b)))
#define XM_CV_ALL     0xFFu        /* 8 个 lane 全相等 */
#else
#if defined(__AVX2__)
typedef __m256i  xm_cv;
#define XM_CV         32u
#define XM_CV_LOADU(p) _mm256_loadu_si256((const __m256i_u *)(const void *)(p))
#define XM_CV_EQ(a,b) ((unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8((a), (b))))
#define XM_CV_ALL     0xFFFFFFFFu
#else
typedef __m128i  xm_cv;
#define XM_CV         16u
#define XM_CV_LOADU(p) _mm_loadu_si128((const __m128i_u *)(const void *)(p))
#define XM_CV_EQ(a,b) ((unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8((a), (b))))
#define XM_CV_ALL     0xFFFFu
#endif
#endif

/* ============================================================================
 * 由"块内相等掩码"求出首个差异字节的差值
 * ==========================================================================*/
static inline int xm_diff_at(const uint8_t *a, const uint8_t *b, unsigned eq, int equality)
{
    if (equality == 0) return -1;

#if defined(__AVX512F__)
    /* 掩码按 64 位 lane; 先定 lane 再定字节 (小端: ctz 即内存序) */
    size_t off = (size_t)__builtin_ctz(~eq & XM_CV_ALL) * 8u;
    const uint8_t *pa = a + off;
    const uint8_t *pb = b + off;
    size_t i = (size_t)__builtin_ctzll(XM_LD64(pa) ^ XM_LD64(pb)) / 8u;
    return (int)pa[i] - (int)pb[i];
#else
    size_t off = (size_t)__builtin_ctz(~eq & XM_CV_ALL);
    return (int)a[off] - (int)b[off];
#endif
}

/* ============================================================================
 * 小块比较: n < 64, 绝不跨越 [a, a+n) 读取
 * ==========================================================================*/
static int xm_cmp_small(const uint8_t *a, const uint8_t *b, size_t n, int equality)
{
    while (n >= 8) {
        uint64_t x = XM_LD64(a), y = XM_LD64(b);
        if (x != y) {
            if (equality == 0) return -1;
            size_t i = (size_t)__builtin_ctzll(x ^ y) / 8u;
            return (int)a[i] - (int)b[i];
        }
        a += 8; b += 8; n -= 8;
    }
    if (n >= 4) {
        uint32_t x = XM_LD32(a), y = XM_LD32(b);
        if (x != y) {
            if (equality == 0) return -1;
            size_t i = (size_t)__builtin_ctz(x ^ y) / 8u;
            return (int)a[i] - (int)b[i];
        }
        a += 4; b += 4; n -= 4;
    }
    if (n >= 2) {
        uint32_t x = (uint32_t)XM_LD16(a), y = (uint32_t)XM_LD16(b);
        if (x != y) {
            if (equality == 0) return -1;
            size_t i = (size_t)__builtin_ctz(x ^ y) / 8u;
            return (int)a[i] - (int)b[i];
        }
        a += 2; b += 2; n -= 2;
    }
    if (n) {
        if (*a != *b) return (equality == 0) ? -1 : ((int)*a - (int)*b);
    }
    return 0;
}

/* ============================================================================
 * 主比较循环: 前置条件 n >= XM_CV
 * ==========================================================================*/
static int xm_cmp(const uint8_t *a, const uint8_t *b, size_t n, int equality)
{
    while (n >= 2u * XM_CV) {
        xm_cv   v0 = XM_CV_LOADU(a);
        xm_cv   w0 = XM_CV_LOADU(b);
        xm_cv   v1 = XM_CV_LOADU(a + XM_CV);
        xm_cv   w1 = XM_CV_LOADU(b + XM_CV);
        unsigned e0 = XM_CV_EQ(v0, w0);
        unsigned e1 = XM_CV_EQ(v1, w1);
        if (e0 != XM_CV_ALL) return xm_diff_at(a, b, e0, equality);
        if (e1 != XM_CV_ALL) return xm_diff_at(a + XM_CV, b + XM_CV, e1, equality);
        a += 2u * XM_CV;
        b += 2u * XM_CV;
        n -= 2u * XM_CV;
    }

    while (n >= XM_CV) {
        unsigned e = XM_CV_EQ(XM_CV_LOADU(a), XM_CV_LOADU(b));
        if (e != XM_CV_ALL) return xm_diff_at(a, b, e, equality);
        a += XM_CV;
        b += XM_CV;
        n -= XM_CV;
    }

    /* 剩余 1 .. XM_CV-1 字节: 只读窗口 [end-XM_CV, end) 恒在界内 */
    if (n) {
        const uint8_t *ta = a + n - XM_CV;
        const uint8_t *tb = b + n - XM_CV;
        unsigned e = XM_CV_EQ(XM_CV_LOADU(ta), XM_CV_LOADU(tb));
        if (e != XM_CV_ALL) return xm_diff_at(ta, tb, e, equality);
    }
    return 0;
}

/* ============================================================================
 * 主入口
 * ==========================================================================*/
int x86memlib_DeclFunction(AVX_memcmp)(const void *str1, const void *str2, size_t numbytes, int equality)
{
    const uint8_t *a = (const uint8_t *)str1;
    const uint8_t *b = (const uint8_t *)str2;

    if (numbytes == 0) return 0;
    if (a == b)        return 0;

    if (numbytes < XM_CV) return xm_cmp_small(a, b, numbytes, equality);
    return xm_cmp(a, b, numbytes, equality);
}

#endif /* __x86_64__ */

/* ============================================================================
 * 多版本弱符号
 * ==========================================================================*/

#ifdef X86MEM_NOT_COMPILE_AVX512
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 0;
__attribute__((weak, used)) int AVX_memcmpV3(const void *a, const void *b, size_t n, int e) {(void)a;(void)b;(void)n;(void)e;return 0;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX2
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 0;
__attribute__((weak, used)) int AVX_memcmpV2(const void *a, const void *b, size_t n, int e) {(void)a;(void)b;(void)n;(void)e;return 0;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 0;
__attribute__((weak, used)) int AVX_memcmpV1(const void *a, const void *b, size_t n, int e) {(void)a;(void)b;(void)n;(void)e;return 0;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 1;
#endif
