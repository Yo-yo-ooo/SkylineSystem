//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * tests/memops — ablib::x86mem 吞吐基准 (对照 libc 与 V0/V1/V2 各 tier)
 *
 * 运行: make -C tests memops-bench && ./tests/bin/memops_bench
 * 输出: 每个尺寸一组的 GB/s (越大越好); libc 列为参照系。
 * ==========================================================================*/
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <vector>

/* MEMOPS_SupportVn 的强定义见 memops_shim.c (独立 TU, 原因见该文件注释) */
#include <x86mem.h>

#ifdef _WIN32
#include <malloc.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

using Clock = std::chrono::steady_clock;

static void *alloc64(size_t sz)
{
#ifdef _WIN32
    return _aligned_malloc(sz, 64);
#else
    void *p = nullptr;
    if (posix_memalign(&p, 64, sz) != 0) return nullptr;
    return p;
#endif
}
static void free64(void *p)
{
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}

#if defined(__x86_64__) || defined(__i386__)
static bool cpu_has(unsigned leaf, unsigned sub, unsigned reg, unsigned bit)
{
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid_count(leaf, sub, &a, &b, &c, &d) == 0) return false;
    unsigned v[4] = {a, b, c, d};
    return (v[reg] >> bit) & 1u;
}
static bool cpu_sse42(void) { return cpu_has(1, 0, 2, 20); }
static bool cpu_avx_ok(void)
{
    if (!cpu_has(1, 0, 2, 28)) return false;   /* ECX.AVX */
    if (!cpu_has(1, 0, 2, 27)) return false;   /* ECX.OSXSAVE */
    unsigned eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((eax & 0x6u) == 0x6u);
}
static bool cpu_avx2(void)    { return cpu_avx_ok() && cpu_has(7, 0, 1, 5); }
static bool cpu_avx512f(void) { return cpu_avx_ok() && cpu_has(7, 0, 1, 16); }
#else
static bool cpu_sse42(void)   { return false; }
static bool cpu_avx_ok(void)  { return false; }
static bool cpu_avx2(void)    { return false; }
static bool cpu_avx512f(void) { return false; }
#endif

struct Impl {
    const char *name;
    void *(*cpy)(void *, void *, size_t);
    void *(*set)(void *, uint8_t, size_t);
    void *(*mv )(void *, void *, size_t);
    int  (*cmp)(const void *, const void *, size_t, int);
};

static void *libc_cpy(void *d, void *s, size_t n)          { return std::memcpy(d, s, n); }
static void *libc_set(void *d, uint8_t c, size_t n)        { return std::memset(d, c, n); }
static void *libc_mv (void *d, void *s, size_t n)          { return std::memmove(d, s, n); }
static int   libc_cmp(const void *a, const void *b, size_t n, int)
{
    return std::memcmp(a, b, n);
}

static volatile uintptr_t g_sink = 0;

/* 以 lambda 内联调用, 各 tier 与 libc 走完全相同的计时路径 */
template <class F>
static double measure(F body, size_t n, int iters)
{
    body();                                  /* 预热 */
    auto t0 = Clock::now();
    for (int i = 0; i < iters; i++) g_sink ^= (uintptr_t)body();
    auto t1 = Clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();
    return sec > 0 ? (double)n * (double)iters / sec / 1e9 : 0.0;   /* GB/s */
}

/* 重复 3 轮取最好: 宿主机器负载抖动很大, 单次采样的误差能盖过真实差异 */
template <class F>
static double measure_best(F body, size_t n, int iters)
{
    double best = 0.0;
    for (int r = 0; r < 3; r++) {
        double v = measure(body, n, iters);
        if (v > best) best = v;
    }
    return best;
}

/* 被测函数一律通过 volatile 函数指针调用: 否则 GCC 会把"参数循环不变且编译器
 * 认为无副作用"的调用 (典型: memcmp 包装) 提举到循环外, 测出来的 GB/s 会是
 * 物理上不可能的天文数字。volatile 装载强制每轮都做一次真正的间接调用。 */
static void *(* volatile g_cpy)(void *, void *, size_t) = nullptr;
static void *(* volatile g_set)(void *, uint8_t, size_t) = nullptr;
static void *(* volatile g_mv )(void *, void *, size_t) = nullptr;
static int  (* volatile g_cmp)(const void *, const void *, size_t, int) = nullptr;

static int pick_iters(size_t n)
{
    if (n < 4096)      return 300000;
    if (n < 65536)     return 60000;
    if (n < (1 << 20)) return 6000;
    return 600;
}

int main(void)
{
    const size_t BLK = 8u << 20;
    uint8_t *A = (uint8_t *)alloc64(BLK);
    uint8_t *B = (uint8_t *)alloc64(BLK);
    if (!A || !B) { std::printf("alloc failed\n"); return 2; }
    std::memset(A, 0x33, BLK);
    std::memset(B, 0x33, BLK);   /* memcmp 走全等路径 (最坏: 必须扫完全长) */

    std::vector<Impl> v;
    v.push_back(Impl{"libc",     libc_cpy, libc_set, libc_mv, libc_cmp});
    if (cpu_sse42())
        v.push_back(Impl{"V0/SSE42", AVX_memcpyV0, AVX_memsetV0, AVX_memmoveV0, AVX_memcmpV0});
    if (cpu_avx_ok() && MEMOPS_SupportV1)
        v.push_back(Impl{"V1/AVX",   AVX_memcpyV1, AVX_memsetV1, AVX_memmoveV1, AVX_memcmpV1});
    if (cpu_avx2() && MEMOPS_SupportV2)
        v.push_back(Impl{"V2/AVX2",  AVX_memcpyV2, AVX_memsetV2, AVX_memmoveV2, AVX_memcmpV2});
    if (cpu_avx512f() && MEMOPS_SupportV3)
        v.push_back(Impl{"V3/AVX512",AVX_memcpyV3, AVX_memsetV3, AVX_memmoveV3, AVX_memcmpV3});

    /* NT 阈值: 编译期默认 -> CPUID 探测的 L3 容量。基准按探测后的值跑,
     * 这样表格反映的是"真机上会走的路径"。 */
    const size_t thr_default = x86mem_cache_limit;
    x86mem_init_cache_limit();
    std::printf("== x86mem 吞吐基准: 各 tier 明细 (GB/s) ==\n");
    std::printf("NT 阈值: 默认 %zu KB -> L3 探测后 %zu KB (L3 = %zu KB)\n\n",
                thr_default / 1024, x86mem_cache_limit / 1024,
                x86mem_detect_l3_size() / 1024);
    const size_t sizes[] = {16, 64, 256, 1024, 4096, 16384, 65536,
                            262144, 1u << 20, 4u << 20};
    const size_t nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (size_t k = 0; k < v.size(); k++) {
        std::printf("\n-- %s --\n", v[k].name);
        std::printf("  %-9s %10s %10s %10s %10s\n", "size", "memcpy", "memset", "memmove", "memcmp");
        for (size_t si = 0; si < nsizes; si++) {
            size_t n = sizes[si];
            int    it = pick_iters(n);
            g_cpy = v[k].cpy;
            g_set = v[k].set;
            g_mv  = v[k].mv;
            g_cmp = v[k].cmp;

            double r0 = measure_best([&]() -> void * { return g_cpy(A, B, n); }, n, it);
            double r1 = measure_best([&]() -> void * { return g_set(A, 0x5A, n); }, n, it);
            double r2 = measure_best([&]() -> void * { return g_mv(A + 64, A + 64 + n / 2, n); }, n, it);

            /* memcmp 必须测"全等"的最坏情形 (强制扫完全长): 上面 set/mv 已经
             * 把 A 的前 n 字节改掉了, 不重新对齐 A/B 的话 memcmp 会在第 0 字节
             * 就返回, 测出来的是调用开销而不是吞吐。 */
            std::memcpy(B, A, n);
            double r3 = measure_best([&]() -> int { return g_cmp(A, B, n, 1); }, n, it);
            std::printf("  %-9zu %10.2f %10.2f %10.2f %10.2f\n", n, r0, r1, r2, r3);
        }
    }

    /* NT 开/关对照: 阈值改成运行时变量后, "多大尺寸才该用 streaming store"
     * 直接由 L3 决定。这一节把阈值钉成两个极端, 看同一尺寸在两条路径上的
     * 实际差距 —— 用来判断当前阈值定得合不合适。 */
    if (v.size() >= 2) {
        const size_t kept = x86mem_cache_limit;
        const Impl  &im   = v[v.size() - 1];          /* 最高 tier */
        std::printf("\n-- NT 开/关对照 (%s, GB/s) --\n", im.name);
        std::printf("  %-9s %12s %12s\n", "size", "NT关(普通)", "NT开(streaming)");
        for (size_t n : {(size_t)262144, (size_t)524288, (size_t)(1u << 20),
                         (size_t)(2u << 20), (size_t)(4u << 20)}) {
            int it = pick_iters(n);
            g_cpy = im.cpy; g_set = im.set; g_mv = im.mv;

            x86mem_cache_limit = (size_t)-1;          /* 关: 任何尺寸都不用 NT */
            double off_c = measure_best([&]() -> void * { return g_cpy(A, B, n); }, n, it);
            double off_s = measure_best([&]() -> void * { return g_set(A, 0x5A, n); }, n, it);

            x86mem_cache_limit = 1;                   /* 开: 任何尺寸都用 NT */
            double on_c = measure_best([&]() -> void * { return g_cpy(A, B, n); }, n, it);
            double on_s = measure_best([&]() -> void * { return g_set(A, 0x5A, n); }, n, it);

            std::printf("  %-9zu %6.2f/%-5.2f %6.2f/%-5.2f\n",
                        n, off_c, off_s, on_c, on_s);
        }
        std::printf("  (格式: memcpy/memset; 当前 L3 阈值 = %zu KB)\n", kept / 1024);
        x86mem_cache_limit = kept;
    }

    free64(A); free64(B);
    return 0;
}
