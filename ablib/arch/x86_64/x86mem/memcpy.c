//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* ============================================================================
 * x86mem :: memcpy —— 分层实现 (SSE4.2 / AVX / AVX2 / AVX512F)
 *
 * 相对旧版阶梯式实现的关键改动:
 *  1. 小尺寸 (<64B) 走"首尾重叠块"路径: 单次分支链, 无函数调用, 且严格
 *     保证所有写入落在 [d, d+n) 内。
 *  2. 中/大尺寸: 先把目的地址对齐到向量宽度, 再走 4x 向量展开主循环, 尾部用
 *     "末尾重叠向量"收尾 —— 全程无标量字节循环。旧版每轮都重走一遍
 *     if-else 阶梯 (每 4KB 一次), 大 buffer 上分支开销显著。
 *  3. 超大尺寸 (>x86mem_cache_limit) 主循环改用非时序 (NT) 存储, 避免冲刷
 *     工作集; 结尾补 sfence 保证跨核可见性。阈值是运行时变量, 由启动代码
 *     用 CPUID 探测的 L3 容量填充 (见 x86mem_init.c)。
 *  4. 一律使用非对齐加载 (现代微架构跨 cacheline 加载代价可忽略)。旧版用的
 *     lddqu / stream_load 在 Haswell 及以后无收益甚至更慢。
 * ==========================================================================*/
#include "./x86mem.h"
#include <stdint.h>
#include <stddef.h>
#include <emmintrin.h>

#ifdef __x86_64__

/* 注意: 阈值不再是编译期常量 —— x86mem_cache_limit 由启动代码按 L3 容量填 */



/* 未对齐标量访问类型 (may_alias, 避免严格别名违规) */
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

#define XM_ST16(p,v)  (*(xm_u16 *)(p) = (v))
#define XM_LD16(p)    (*(const xm_u16 *)(p))
#define XM_ST32(p,v)  (*(xm_u32 *)(p) = (v))
#define XM_LD32(p)    (*(const xm_u32 *)(p))
#define XM_ST64(p,v)  (*(xm_u64 *)(p) = (v))
#define XM_LD64(p)    (*(const xm_u64 *)(p))

#define XM_V128_LOADU(p)   _mm_loadu_si128((const __m128i_u *)(const void *)(p))
#define XM_V128_STOREU(p,v) _mm_storeu_si128((__m128i_u *)(void *)(p), (v))

/* 主循环展开块: N 次加载全部先于 N 次存储。
 * 展开度实测敏感: 4x 在本仓库目标机器 (Alder Lake) 上最稳; 8x 会因 store
 * buffer 与调度压力反而掉 ~15%。改这个值必须重跑 tests/memops 基准。 */
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
 * 小块拷贝: n < 64, 用"首块 + 尾块"夹逼, 保证写入不越出 [d, d+n)
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

    /* 16 <= n < 64: 16 字节块首尾夹逼。
     * 覆盖 [0,16)  U [n-16,n)        -> n <= 32 完备
     * 再加 [16,32) U [n-32,n-16)     -> n <  64 完备 */
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
 * 主拷贝循环: 前置条件 n >= XM_VEC
 *   - 先用一个未对齐向量把 d 推到 XM_VEC 对齐 (写入的 XM_VEC 字节一定在界内)
 *   - 4x 向量展开主循环
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

    /* 剩余 1 .. XM_VEC-1 字节: 读/写窗口 [end-XM_VEC, end) 恒在界内
     * (因为入口处 n_orig >= XM_VEC) */
    if (n) XM_STOREU(d + n - XM_VEC, XM_LOADU(s + n - XM_VEC));

    if (nt) _mm_sfence();
}

/* ============================================================================
 * 主入口
 * ==========================================================================*/
#ifdef OVERLAP_CHECK
void *x86memlib_UseFunction(AVX_memmove)(void *dest, void *src, size_t numbytes);
#endif

void * x86memlib_DeclFunction(AVX_memcpy)(void *dest, void *src, size_t numbytes)
{
    uint8_t       *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    if (numbytes == 0) return dest;
    if (d == s)        return dest;

#ifdef OVERLAP_CHECK
    if ((d > s && d < s + numbytes) || (s > d && s < d + numbytes))
        return x86memlib_UseFunction(AVX_memmove)(dest, src, numbytes);
#endif

    if (numbytes < XM_VEC) {
        xm_copy_small(d, s, numbytes);
        return dest;
    }

    xm_copy_fwd(d, s, numbytes, numbytes > x86mem_cache_limit);
    return dest;
}

#endif /* __x86_64__ */

/* ============================================================================
 * 多版本弱符号: 未参与编译的 tier 提供空桩, 使 V0..V3 符号恒存在
 * ==========================================================================*/

#ifdef X86MEM_NOT_COMPILE_AVX512
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 0;
__attribute__((weak, used)) void *AVX_memcpyV3(void *dest, void *src, size_t n) {(void)src;(void)n;return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV3 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX2
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 0;
__attribute__((weak, used)) void *AVX_memcpyV2(void *dest, void *src, size_t n) {(void)src;(void)n;return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV2 = 1;
#endif

#ifdef X86MEM_NOT_COMPILE_AVX
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 0;
__attribute__((weak, used)) void *AVX_memcpyV1(void *dest, void *src, size_t n) {(void)src;(void)n;return dest;}
#else
__attribute__((weak)) uint8_t MEMOPS_SupportV1 = 1;
#endif
