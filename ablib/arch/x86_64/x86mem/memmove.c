//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* ============================================================================
 * x86mem :: memmove —— 分层实现 (SSE4.2 / AVX / AVX2 / AVX512F)
 *
 * 旧版为 4098 行、逐个块函数 (memmove_512bit_2kB_as 等) 展开的阶梯式实现,
 * 且正/反向各有一套 200+ 行的 if-else 阶梯, 每 4KB 重走一次。
 * 新实现:
 *   - d < s (dst 在前): 若 gap >= 向量宽度 走对齐 + 4x 展开快路径;
 *     否则走"严格自低向高、每组先全读再全写"的顺序路径 —— 这是重叠安全的
 *     唯一正确形式 (首尾夹逼在重叠时会读到已被自己覆写的源字节)。
 *   - d >= s+n: 完全不重叠, 直接走 memcpy 同款快路径。
 *   - s < d < s+n: 反向内核, 先把尾端对齐到向量边界, 再向下 4x 展开,
 *     同样每组先全读再全写。
 *   - 仅当两块完全不相交时才启用 NT 存储 (重叠时 NT 写后立刻读回同一行
 *     会退化成一次内存往返)。
 *
 * 重叠安全性不变量 (gap = |d - s|):
 *   正向: 写窗口恒在当前读窗口之下, 且同一组内 4 次加载全部先于 4 次存储;
 *   反向: 写窗口恒在当前读窗口之上, 同一组内同样先全读后全写。
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
#define XM_LOADU(p)    _mm512_loadu_si512((const void *)(p))
#define XM_STOREU(p,v) _mm512_storeu_si512((void *)(p), (v))
#define XM_STOREA(p,v) _mm512_store_si512((void *)(p), (v))
#define XM_STORENT(p,v) _mm512_stream_si512((__m512i *)(void *)(p), (v))
#elif defined(__AVX__)
typedef __m256i   xm_vec;
#define XM_VEC         32u
#define XM_LOADU(p)    _mm256_loadu_si256((const __m256i_u *)(const void *)(p))
#define XM_STOREU(p,v) _mm256_storeu_si256((__m256i_u *)(void *)(p), (v))
#define XM_STOREA(p,v) _mm256_store_si256((__m256i *)(void *)(p), (v))
#define XM_STORENT(p,v) _mm256_stream_si256((__m256i *)(void *)(p), (v))
#else
typedef __m128i   xm_vec;
#define XM_VEC         16u
#define XM_LOADU(p)    _mm_loadu_si128((const __m128i_u *)(const void *)(p))
#define XM_STOREU(p,v) _mm_storeu_si128((__m128i_u *)(void *)(p), (v))
#define XM_STOREA(p,v) _mm_store_si128((__m128i *)(void *)(p), (v))
#define XM_STORENT(p,v) _mm_stream_si128((__m128i *)(void *)(p), (v))
#endif

#define XM_LD16(p)    (*(const xm_u16 *)(p))
#define XM_ST16(p,v)  (*(xm_u16 *)(p) = (v))
#define XM_LD32(p)    (*(const xm_u32 *)(p))
#define XM_ST32(p,v)  (*(xm_u32 *)(p) = (v))
#define XM_LD64(p)    (*(const xm_u64 *)(p))
#define XM_ST64(p,v)  (*(xm_u64 *)(p) = (v))

#define XM_V128_LOADU(p)    _mm_loadu_si128((const __m128i_u *)(const void *)(p))
#define XM_V128_STOREU(p,v) _mm_storeu_si128((__m128i_u *)(void *)(p), (v))

/* 主循环展开块: N 次加载全部先于 N 次存储 (重叠安全的必要条件)。
 * 展开度实测敏感: 4x 在目标机器 (Alder Lake) 上最稳, 8x 反而掉 ~15%。
 * 改这个值必须重跑 tests/memops 基准。 */
#define XM_BLK(STORE)                                                      \
    do {                                                                   \
        xm_vec v0 = XM_LOADU(s);                                           \
        xm_vec v1 = XM_LOADU(s + XM_VEC);                                  \
        xm_vec v2 = XM_LOADU(s + 2u * XM_VEC);                             \
        xm_vec v3 = XM_LOADU(s + 3u * XM_VEC);                             \
        STORE(d,               v0); STORE(d + XM_VEC,      v1);            \
        STORE(d + 2u * XM_VEC, v2); STORE(d + 3u * XM_VEC, v3);            \
    } while (0)

/* ============================================================================
 * 非重叠小块拷贝 (n < 64): 首尾夹逼, 写入严格落在 [d, d+n)
 * 仅用于 memcpy / memmove 的"不相交"分支
 * ==========================================================================*/
static inline void xm_copy_small(uint8_t *d, const uint8_t *s, size_t n)
{
    if (n == 0) return;

    if (n <= 15) {
        if (n < 8) {
            if (n < 4) {
                if (n < 2) { d[0] = s[0]; return; }
                XM_ST16(d, XM_LD16(s));
                XM_ST16(d + n - 2, XM_LD16(s + n - 2));
                return;
            }
            XM_ST32(d, XM_LD32(s));
            XM_ST32(d + n - 4, XM_LD32(s + n - 4));
            return;
        }
        XM_ST64(d, XM_LD64(s));
        XM_ST64(d + n - 8, XM_LD64(s + n - 8));
        return;
    }

    __m128i a0 = XM_V128_LOADU(s);
    __m128i z0 = XM_V128_LOADU(s + n - 16);
    XM_V128_STOREU(d, a0);
    XM_V128_STOREU(d + n - 16, z0);
    if (n > 32) {
        __m128i a1 = XM_V128_LOADU(s + 16);
        __m128i z1 = XM_V128_LOADU(s + n - 32);
        XM_V128_STOREU(d + 16, a1);
        XM_V128_STOREU(d + n - 32, z1);
    }
}

/* ============================================================================
 * 重叠安全的正向拷贝: d 恒在 s 之前 (或不相交), 严格自低向高,
 * 每组 4 个向量先全部加载再全部存储
 * ==========================================================================*/
/* 重叠正向 / 反向都标 noinline: 主入口若把三条路径全内联进来会膨胀到 ~1.3KB,
 * 小尺寸调用的 I-cache 命中率明显变差 (实测 64B memmove 掉近一半吞吐)。
 * 这两条是冷路径 —— 真正的大块搬运都走下面的 xm_copy_fwd。 */
__attribute__((noinline))
static void xm_copy_fwd_seq(uint8_t *d, const uint8_t *s, size_t n)
{
    while (n >= 4u * XM_VEC) {
        XM_BLK(XM_STOREU);
        d += 4u * XM_VEC;
        s += 4u * XM_VEC;
        n -= 4u * XM_VEC;
    }
    while (n >= XM_VEC) {
        XM_STOREU(d, XM_LOADU(s));
        d += XM_VEC;
        s += XM_VEC;
        n -= XM_VEC;
    }
    while (n >= 16) {
        XM_V128_STOREU(d, XM_V128_LOADU(s));
        d += 16; s += 16; n -= 16;
    }
    if (n >= 8) { XM_ST64(d, XM_LD64(s)); d += 8; s += 8; n -= 8; }
    if (n >= 4) { XM_ST32(d, XM_LD32(s)); d += 4; s += 4; n -= 4; }
    if (n >= 2) { XM_ST16(d, XM_LD16(s)); d += 2; s += 2; n -= 2; }
    if (n)      { *d = *s; }
}

/* ============================================================================
 * 快路径正向拷贝: 前置条件 n >= XM_VEC 且 (不相交 或 gap >= XM_VEC)
 *   - 先用一个未对齐向量把 d 推到 XM_VEC 对齐 (该写入的 XM_VEC 字节恒在界内,
 *     且因 gap >= XM_VEC, 不会污染尚未读取的源字节)
 *   - 4x 向量展开 (先全读后全写)
 *   - 收尾: 一次"末尾重叠"的向量存储
 * ==========================================================================*/
static void xm_copy_fwd(uint8_t *d, const uint8_t *s, size_t n, int nt)
{
    size_t mis = (size_t)((uintptr_t)d & (XM_VEC - 1u));

    if (mis) {
        size_t head = XM_VEC - mis;      /* 1 .. XM_VEC-1, 恒 < n */
        XM_STOREU(d, XM_LOADU(s));
        d += head;
        s += head;
        n -= head;
    }
    /* 此后 d 恒按 XM_VEC 对齐 */

    if (nt) {
        while (n >= 4u * XM_VEC) {
            XM_BLK(XM_STORENT);
            d += 4u * XM_VEC;
            s += 4u * XM_VEC;
            n -= 4u * XM_VEC;
        }
    } else {
        while (n >= 4u * XM_VEC) {          /* d 已对齐: 用对齐存储 */
            XM_BLK(XM_STOREA);
            d += 4u * XM_VEC;
            s += 4u * XM_VEC;
            n -= 4u * XM_VEC;
        }
    }

    while (n >= XM_VEC) {
        XM_STOREA(d, XM_LOADU(s));
        d += XM_VEC;
        s += XM_VEC;
        n -= XM_VEC;
    }

    /* 剩余 1 .. XM_VEC-1 字节: 窗口 [end-XM_VEC, end) 恒在界内 */
    if (n) XM_STOREU(d + n - XM_VEC, XM_LOADU(s + n - XM_VEC));

    if (nt) _mm_sfence();
}

/* ============================================================================
 * 反向小块拷贝: d/s 指区间的"尾后"位置, 向低地址写 n 字节,
 * 写入严格落在 [d-n, d)
 * ==========================================================================*/
static inline void xm_copy_small_bwd(uint8_t *d, const uint8_t *s, size_t n)
{
    while (n >= 16) {
        d -= 16; s -= 16; n -= 16;
        XM_V128_STOREU(d, XM_V128_LOADU(s));
    }
    if (n >= 8) { d -= 8; s -= 8; n -= 8; XM_ST64(d, XM_LD64(s)); }
    if (n >= 4) { d -= 4; s -= 4; n -= 4; XM_ST32(d, XM_LD32(s)); }
    if (n >= 2) { d -= 2; s -= 2; n -= 2; XM_ST16(d, XM_LD16(s)); }
    if (n)      { --d; --s; *d = *s; }
}

/* ============================================================================
 * 反向主循环 (处理 s < d < s+n 的重叠情形)
 * ==========================================================================*/
__attribute__((noinline))
static void xm_copy_bwd(uint8_t *d, const uint8_t *s, size_t n)
{
    d += n;
    s += n;

    if (n >= XM_VEC) {
        /* 先把顶部 mis 字节搬掉, 使 d 落到 XM_VEC 对齐边界上。
         * 注意: 不能像正向那样"先存一个整向量再回退", 那会写到区间之外。 */
        size_t mis = (size_t)((uintptr_t)d & (XM_VEC - 1u));
        if (mis) {
            size_t top = mis;
            d -= mis;
            s -= mis;
            n -= mis;
            xm_copy_small_bwd(d + top, s + top, top);   /* [d, d+top) <- [s, s+top) */
        }

        /* 每组 4 个向量先全部加载再全部存储: 反向重叠下这是唯一安全顺序 */
        while (n >= 4u * XM_VEC) {
            d -= 4u * XM_VEC;
            s -= 4u * XM_VEC;
            XM_BLK(XM_STOREA);
            n -= 4u * XM_VEC;
        }

        while (n >= XM_VEC) {
            d -= XM_VEC;
            s -= XM_VEC;
            XM_STOREA(d, XM_LOADU(s));
            n -= XM_VEC;
        }
    }

    xm_copy_small_bwd(d, s, n);
}

/* ============================================================================
 * 主入口
 * ==========================================================================*/
void * x86memlib_DeclFunction(AVX_memmove)(void *dest, void *src, size_t numbytes)
{
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (numbytes == 0) return dest;
    if (d == s)        return dest;

    if (d < s) {
        /* dst 在前: 只有 gap 够大、且长度够摊薄循环开销时才走"先对齐目的
         * 地址"的快路径; 否则用严格顺序拷贝 (首尾夹逼在重叠时会读到自己
         * 刚覆写的源字节, 绝不能用) */
        if (numbytes >= 4u * XM_VEC && (size_t)(s - d) >= XM_VEC) {
            int nt = (numbytes > x86mem_cache_limit) && (d + numbytes <= s);
            xm_copy_fwd(d, s, numbytes, nt);
        } else {
            xm_copy_fwd_seq(d, s, numbytes);
        }
        return dest;
    }

    if (d >= s + numbytes) {
        /* dst 在 src 之后且完全不相交: 与 memcpy 同路径 */
        if (numbytes < XM_VEC) {
            xm_copy_small(d, s, numbytes);
            return dest;
        }
        xm_copy_fwd(d, s, numbytes, numbytes > x86mem_cache_limit);
        return dest;
    }

    /* s < d < s + numbytes: 必须从尾端往回搬 */
    xm_copy_bwd(d, s, numbytes);
    return dest;
}

#endif /* __x86_64__ */

/* ============================================================================
 * 多版本弱符号
 * ==========================================================================*/

#ifdef X86MEM_NOT_COMPILE_AVX512
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 0;
__attribute__((weak, used)) void *AVX_memmoveV3(void *dest, void *src, size_t n) {(void)src;(void)n;return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX2
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 0;
__attribute__((weak, used)) void *AVX_memmoveV2(void *dest, void *src, size_t n) {(void)src;(void)n;return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 0;
__attribute__((weak, used)) void *AVX_memmoveV1(void *dest, void *src, size_t n) {(void)src;(void)n;return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 1;
#endif
