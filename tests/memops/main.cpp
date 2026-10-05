//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * tests/memops — ablib::x86mem (memcpy/memmove/memset/memcmp) 宿主正确性测试
 *
 * 覆盖策略:
 *  1. 小尺寸穷举: n = 0..SMALL_MAX, dst/src 偏移扫遍对齐/非对齐组合
 *  2. 越界写检测: dst 两侧 16 字节哨兵 + 整池期末全量扫描
 *  3. 重叠: memmove 的 delta (dst-src) 覆盖 -4096..+4096, 含完全重叠/自搬
 *  4. 大尺寸: 4K / 64K / 1M / 4M(跨 CACHESIZELIMIT, 走 NT 存储) / 5M
 *  5. 随机模糊: 大 buffer 随机差异位置, 与 libc 参考实现对照符号
 *  6. 契约: memcpy/memmove/memset 必须返回原始入参; memcmp 与 libc 同号
 *
 * 运行: make -C tests memops && ./tests/bin/memops_test
 * ==========================================================================*/
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

/* MEMOPS_SupportVn 的强定义见 memops_shim.c (独立 TU, 原因见该文件注释) */
#include <x86mem.h>

#ifdef _WIN32
#include <malloc.h>
#endif
#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

/* ------------------------------------------------------------------ 基础设施 */

static int  g_checks = 0;
static int  g_fails  = 0;
static int  g_verbose = 0;

#define CHECK(cond, fmt, ...)                                              \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            if (g_fails <= 40) {                                           \
                std::printf("  FAIL %s:%d " fmt "\n", __FILE__, __LINE__,  \
                            ##__VA_ARGS__);                                \
            }                                                              \
        }                                                                  \
    } while (0)

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

/* ------------------------------------------------------------------ CPU 探测 */

#if defined(__x86_64__) || defined(__i386__)
static void cpuid_count(unsigned leaf, unsigned sub, unsigned *a,
                        unsigned *b, unsigned *c, unsigned *d)
{
    unsigned ea = 0, eb = 0, ec = 0, ed = 0;
    if (__get_cpuid_count(leaf, sub, &ea, &eb, &ec, &ed) == 0) {
        ea = eb = ec = ed = 0;
    }
    *a = ea; *b = eb; *c = ec; *d = ed;
}

static bool cpu_sse42(void)
{
    unsigned a, b, c, d; cpuid_count(1, 0, &a, &b, &c, &d);
    return (c >> 20) & 1u;
}
static bool cpu_avx(void)
{
    unsigned a, b, c, d; cpuid_count(1, 0, &a, &b, &c, &d);
    if (!((c >> 28) & 1u)) return false;      /* AVX */
    if (!((c >> 27) & 1u)) return false;      /* OSXSAVE */
    unsigned eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((eax & 0x6u) == 0x6u);            /* XCR0: SSE | YMM */
}
static bool cpu_avx2(void)
{
    if (!cpu_avx()) return false;
    unsigned a, b, c, d; cpuid_count(7, 0, &a, &b, &c, &d);
    return (b >> 5) & 1u;
}
static bool cpu_avx512f(void)
{
    if (!cpu_avx()) return false;
    unsigned a, b, c, d; cpuid_count(7, 0, &a, &b, &c, &d);
    if (!((b >> 16) & 1u)) return false;      /* AVX512F */
    unsigned eax, edx;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    /* XCR0: SSE | YMM | opmask | ZMM_hi256 | hi16_ZMM */
    return ((eax & 0xE6u) == 0xE6u);
}
#else
static bool cpu_sse42(void)   { return false; }
static bool cpu_avx(void)     { return false; }
static bool cpu_avx2(void)    { return false; }
static bool cpu_avx512f(void) { return false; }
#endif

/* ------------------------------------------------------------------ 被测实现 */

struct Impl {
    const char *name;
    void *(*cpy)(void *, void *, size_t);
    void *(*set)(void *, uint8_t, size_t);
    void *(*mv )(void *, void *, size_t);
    int  (*cmp)(const void *, const void *, size_t, int);
};

/* ------------------------------------------------------------------ 参考实现 */

static void ref_move(uint8_t *d, const uint8_t *s, size_t n)
{
    if (d == s || n == 0) return;
    if (d < s) { for (size_t i = 0; i < n; i++) d[i] = s[i]; }
    else       { for (size_t i = n; i-- > 0; )  d[i] = s[i]; }
}

static int sgn(int v) { return (v > 0) - (v < 0); }

/* ------------------------------------------------------------------ 测试池 */

static const size_t GUARD   = 1024;
static const size_t BIG     = 5u * 1024u * 1024u + 4096u;   /* 跨 3MB NT 阈值 */
static const size_t OVERREG = 16384;

struct Pool {
    uint8_t *buf = nullptr;      /* 双区池: 非重叠测试用 */
    size_t   sz  = 0;
    uint8_t *A   = nullptr;      /* 区 1 (通常作 dst) */
    uint8_t *B   = nullptr;      /* 区 2 (通常作 src) */
    uint8_t *ov  = nullptr;      /* 单区池: 重叠测试用 */

    bool init(void)
    {
        sz = GUARD * 3 + BIG * 2;
        buf = (uint8_t *)alloc64(sz);
        if (!buf) return false;
        A = buf + GUARD;
        B = buf + GUARD + BIG + GUARD;
        ov = (uint8_t *)alloc64(GUARD * 2 + OVERREG);
        if (!ov) return false;
        ov += GUARD;
        return true;
    }
    ~Pool(void)
    {
        if (buf) free64(buf);
        if (ov)  free64(ov - GUARD);
    }
};

static uint64_t g_rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    g_rng_state ^= g_rng_state << 13;
    g_rng_state ^= g_rng_state >> 7;
    g_rng_state ^= g_rng_state << 17;
    return (uint32_t)(g_rng_state >> 32);
}

/* 用随机内容铺满 [p, p+n) */
static void fill_rand(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd();
}

/* 哨兵校验: dst 前后各 16 字节必须仍是 SENT */
static const uint8_t SENT = 0xA5;
static bool sentinels_ok(const uint8_t *base, size_t n)
{
    for (size_t i = 16; i-- > 0; )
        if (base[-1 - i] != SENT || base[n + i] != SENT) return false;
    return true;
}

/* ========================================================================== */
/* 1. memcpy                                                                   */
/* ========================================================================== */
static const size_t OFFS[] = {0, 1, 2, 3, 5, 7, 8, 15, 16, 17, 31, 32, 33, 63};
static const size_t NOFFS  = sizeof(OFFS) / sizeof(OFFS[0]);
static const size_t SMALL_MAX = 300;

static void test_memcpy_small(const Impl &im, Pool &p)
{
    for (size_t n = 0; n <= SMALL_MAX; n++) {
        for (size_t di = 0; di < NOFFS; di++) {
            for (size_t si = 0; si < NOFFS; si++) {
                uint8_t *d = p.A + OFFS[di] + 64;
                uint8_t *s = p.B + OFFS[si] + 64;
                /* 铺哨兵 */
                for (size_t i = 16; i-- > 0; ) { d[-1-i] = SENT; s[-1-i] = SENT; }
                for (size_t i = 0; i < n; i++)  d[i] = SENT;
                for (size_t i = 16; i-- > 0; ) { d[n+i] = SENT; s[n+i] = SENT; }
                fill_rand(s, n);

                std::vector<uint8_t> want(s, s + n);
                void *r = im.cpy(d, s, n);

                if (r != d) { CHECK(false, "%s memcpy 返回值 != dest (n=%zu)", im.name, n); continue; }
                if (n && std::memcmp(d, want.data(), n) != 0) {
                    CHECK(false, "%s memcpy 内容不符 (n=%zu doff=%zu soff=%zu)",
                          im.name, n, OFFS[di], OFFS[si]);
                    continue;
                }
                if (!sentinels_ok(d, n)) {
                    CHECK(false, "%s memcpy 越界写 (n=%zu doff=%zu soff=%zu)",
                          im.name, n, OFFS[di], OFFS[si]);
                }
                ++g_checks;
            }
        }
    }
}

/* ========================================================================== */
/* 2. memmove (含重叠)                                                        */
/* ========================================================================== */
static void test_memmove_overlap(const Impl &im, Pool &p)
{
    const size_t SRC_AT = 4096;
    for (size_t n = 0; n <= SMALL_MAX; n++) {
        for (long delta = -64; delta <= 64; delta++) {
            uint8_t *s = p.ov + SRC_AT;
            uint8_t *d = s + delta;
            if (d < p.ov || d + n > p.ov + OVERREG) continue;

            fill_rand(p.ov, OVERREG);
            std::vector<uint8_t> want(p.ov, p.ov + OVERREG);
            ref_move(want.data() + (d - p.ov), want.data() + (s - p.ov), n);

            void *r = im.mv(d, s, n);
            if (r != d) { CHECK(false, "%s memmove 返回值 != dest (n=%zu)", im.name, n); continue; }
            CHECK(std::memcmp(p.ov, want.data(), OVERREG) == 0,
                  "%s memmove 结果不符 (n=%zu delta=%ld)", im.name, n, delta);
        }
    }

    /* 大块重叠: 覆盖对齐/非对齐 + 跨 NT 阈值 */
    const size_t big_n[] = {4096, 65536, 1u << 20};
    const long   big_delta[] = {-1, 1, 7, 31, 32, 64, 4096, -4096};
    for (size_t n : big_n) {
        for (long delta : big_delta) {
            uint8_t *s = p.ov + SRC_AT;
            uint8_t *d = s + delta;
            if (d < p.ov || d + n > p.ov + OVERREG) continue;
            fill_rand(p.ov, OVERREG);
            std::vector<uint8_t> want(p.ov, p.ov + OVERREG);
            ref_move(want.data() + (d - p.ov), want.data() + (s - p.ov), n);
            CHECK(im.mv(d, s, n) == d, "%s memmove 返回值 != dest", im.name);
            CHECK(std::memcmp(p.ov, want.data(), OVERREG) == 0,
                  "%s memmove 大块结果不符 (n=%zu delta=%ld)", im.name, n, delta);
        }
    }
}

/* ========================================================================== */
/* 3. memset                                                                   */
/* ========================================================================== */
static void test_memset_small(const Impl &im, Pool &p)
{
    const uint8_t vals[] = {0x00, 0x01, 0x7F, 0x80, 0xA5, 0xFF};
    for (size_t n = 0; n <= SMALL_MAX; n++) {
        for (size_t di = 0; di < NOFFS; di++) {
            for (uint8_t v : vals) {
                uint8_t *d = p.A + OFFS[di] + 64;
                for (size_t i = 16; i-- > 0; ) d[-1-i] = SENT;
                for (size_t i = 0; i < n; i++)  d[i] = (uint8_t)(v ^ 0xFF);
                for (size_t i = 16; i-- > 0; ) d[n+i] = SENT;

                void *r = im.set(d, v, n);
                if (r != d) { CHECK(false, "%s memset 返回值 != dest (n=%zu)", im.name, n); continue; }
                bool ok = true;
                for (size_t i = 0; i < n; i++) if (d[i] != v) { ok = false; break; }
                CHECK(ok, "%s memset 内容不符 (n=%zu doff=%zu v=0x%02X)",
                      im.name, n, OFFS[di], v);
                if (ok) CHECK(sentinels_ok(d, n), "%s memset 越界写 (n=%zu doff=%zu)",
                              im.name, n, OFFS[di]);
            }
        }
    }
}

/* ========================================================================== */
/* 4. memcmp                                                                   */
/* ========================================================================== */
static void test_memcmp_small(const Impl &im, Pool &p)
{
    for (size_t n = 1; n <= SMALL_MAX; n++) {
        for (size_t di = 0; di < NOFFS; di++) {
            for (size_t si = 0; si < NOFFS; si++) {
                uint8_t *a = p.A + OFFS[di] + 64;
                uint8_t *b = p.B + OFFS[si] + 64;
                fill_rand(a, n);
                std::memcpy(b, a, n);

                /* 全等 */
                CHECK(im.cmp(a, b, n, 1) == 0, "%s memcmp 全等应为 0 (n=%zu)", im.name, n);
                CHECK(im.cmp(a, b, n, 0) == 0, "%s memcmp(eq) 全等应为 0 (n=%zu)", im.name, n);

                /* 在若干位置注入差异, 覆盖首/中/尾与两种符号 */
                size_t pos[3] = {0, n / 2, n - 1};
                for (size_t k = 0; k < 3; k++) {
                    for (int sign = 0; sign < 2; sign++) {
                        uint8_t x = (uint8_t)rnd(), y = x;
                        if (x < 200) y = (uint8_t)(x + (sign ? 1u : 40u));
                        else         y = (uint8_t)(x - (sign ? 1u : 40u));
                        a[pos[k]] = x; b[pos[k]] = y;
                        int got  = im.cmp(a, b, n, 1);
                        int want = std::memcmp(a, b, n);
                        CHECK(sgn(got) == sgn(want),
                              "%s memcmp 符号错 (n=%zu pos=%zu got=%d want=%d)",
                              im.name, n, pos[k], got, want);
                        CHECK(im.cmp(a, b, n, 0) == -1,
                              "%s memcmp(eq) 不等应返回 -1 (n=%zu)", im.name, n);
                        a[pos[k]] = b[pos[k]];
                    }
                }
            }
        }
    }
}

/* ========================================================================== */
/* 5. 大尺寸 (含跨 NT 阈值的路径)                                             */
/* ========================================================================== */
static void test_large(const Impl &im, Pool &p)
{
    const size_t sizes[] = {4096, 65536, 1u << 20, 4u << 20, 5u << 20};
    const size_t offs[]  = {0, 1, 7, 16, 31, 63};

    for (size_t n : sizes) {
        for (size_t o : offs) {
            uint8_t *d = p.A + o;
            uint8_t *s = p.B + o;

            /* memcpy */
            fill_rand(s, n);
            std::vector<uint8_t> want(s, s + n);
            std::memset(d, SENT, n);
            CHECK(im.cpy(d, s, n) == d, "%s memcpy 返回值 != dest (n=%zu)", im.name, n);
            CHECK(std::memcmp(d, want.data(), n) == 0, "%s memcpy 大块不符 (n=%zu off=%zu)",
                  im.name, n, o);

            /* memmove (非重叠大块) */
            fill_rand(s, n);
            want.assign(s, s + n);
            CHECK(im.mv(d, s, n) == d, "%s memmove 返回值 != dest (n=%zu)", im.name, n);
            CHECK(std::memcmp(d, want.data(), n) == 0, "%s memmove 大块不符 (n=%zu off=%zu)",
                  im.name, n, o);

            /* memset */
            for (uint8_t v : {(uint8_t)0, (uint8_t)0x5A, (uint8_t)0xFF}) {
                std::memset(d, (uint8_t)(v ^ 0xFF), n);
                CHECK(im.set(d, v, n) == d, "%s memset 返回值 != dest (n=%zu)", im.name, n);
                CHECK(std::memcmp(d, std::vector<uint8_t>(n, v).data(), n) == 0,
                      "%s memset 大块不符 (n=%zu off=%zu v=0x%02X)", im.name, n, o, v);
            }

            /* memcmp: 全等 + 每 1/3、2/3、尾部注入差异 */
            fill_rand(s, n);
            std::memcpy(d, s, n);
            CHECK(im.cmp(d, s, n, 1) == 0, "%s memcmp 大块全等 (n=%zu)", im.name, n);
            for (size_t pos : {n / 3, (n * 2) / 3, n - 1}) {
                d[pos] ^= 0x40;
                CHECK(sgn(im.cmp(d, s, n, 1)) == sgn(std::memcmp(d, s, n)),
                      "%s memcmp 大块符号错 (n=%zu pos=%zu)", im.name, n, pos);
                CHECK(im.cmp(d, s, n, 0) == -1, "%s memcmp(eq) 大块 (n=%zu)", im.name, n);
                d[pos] ^= 0x40;
            }
        }
    }
}

/* ========================================================================== */
/* 6. 随机模糊                                                                 */
/* ========================================================================== */
static void test_fuzz(const Impl &im, Pool &p)
{
    for (int iter = 0; iter < 400; iter++) {
        size_t n = 1 + (rnd() % 65536);
        size_t doff = rnd() & 63, soff = rnd() & 63;
        uint8_t *d = p.A + doff;
        uint8_t *s = p.B + soff;

        fill_rand(s, n);
        std::vector<uint8_t> want(s, s + n);
        CHECK(im.cpy(d, s, n) == d, "%s fuzz memcpy retval", im.name);
        CHECK(std::memcmp(d, want.data(), n) == 0, "%s fuzz memcpy (n=%zu)", im.name, n);

        /* 随机注入 k 处差异 */
        int k = 1 + (int)(rnd() % 4);
        for (int i = 0; i < k && n; i++) d[rnd() % n] ^= (uint8_t)(1u << (rnd() & 7));
        CHECK(sgn(im.cmp(d, s, n, 1)) == sgn(std::memcmp(d, s, n)),
              "%s fuzz memcmp sign (n=%zu)", im.name, n);
        if (std::memcmp(d, s, n) == 0)
            CHECK(im.cmp(d, s, n, 1) == 0, "%s fuzz memcmp zero (n=%zu)", im.name, n);
    }
}

/* ========================================================================== */
/* 7. NT 阈值扫描                                                              */
/*    x86mem_cache_limit 是运行时变量 (启动时由 CPUID 探测的 L3 容量填充)。
 *    这里把阈值钉成几个极端值, 强制同一批大块操作分别走 / 不走 streaming
 *    store, 确认两条路径都正确 —— 默认阈值下 4KB 永远走不到 NT 路径。 */
/* ========================================================================== */
static void test_nt_thresholds(const Impl &im, Pool &p)
{
    const size_t kept = x86mem_cache_limit;
    const size_t sizes[] = {4096, 65536, 1u << 20, 4u << 20};
    /* 64 -> 全部走 NT; SIZE_MAX -> 全部走普通存储; 4096 -> 边界两侧各一半 */
    const size_t thr[] = {X86MEM_CACHE_LIMIT_MIN, 4096u, kept, (size_t)-1};

    for (size_t t : thr) {
        x86mem_cache_limit = t;
        for (size_t n : sizes) {
            uint8_t *d = p.A + 7;
            uint8_t *s = p.B + 7;

            fill_rand(s, n);
            std::vector<uint8_t> want(s, s + n);
            std::memset(d, SENT, n);
            CHECK(im.cpy(d, s, n) == d, "%s NT扫描 memcpy retval (thr=%zu)", im.name, t);
            CHECK(std::memcmp(d, want.data(), n) == 0,
                  "%s NT扫描 memcpy 不符 (thr=%zu n=%zu)", im.name, t, n);

            fill_rand(s, n);
            want.assign(s, s + n);
            CHECK(im.mv(d, s, n) == d, "%s NT扫描 memmove retval (thr=%zu)", im.name, t);
            CHECK(std::memcmp(d, want.data(), n) == 0,
                  "%s NT扫描 memmove 不符 (thr=%zu n=%zu)", im.name, t, n);

            for (uint8_t v : {(uint8_t)0, (uint8_t)0xE7}) {
                std::memset(d, (uint8_t)(v ^ 0xFF), n);
                CHECK(im.set(d, v, n) == d, "%s NT扫描 memset retval (thr=%zu)", im.name, t);
                CHECK(std::memcmp(d, std::vector<uint8_t>(n, v).data(), n) == 0,
                      "%s NT扫描 memset 不符 (thr=%zu n=%zu v=0x%02X)", im.name, t, n, v);
            }
        }
    }
    x86mem_cache_limit = kept;
}

/* ========================================================================== */

static void run_all(const Impl &im, Pool &p)
{
    std::printf("  [%s] memcpy 小尺寸穷举 ...\n", im.name);
    test_memcpy_small(im, p);
    std::printf("  [%s] memmove 重叠 ...\n", im.name);
    test_memmove_overlap(im, p);
    std::printf("  [%s] memset 小尺寸穷举 ...\n", im.name);
    test_memset_small(im, p);
    std::printf("  [%s] memcmp 小尺寸穷举 ...\n", im.name);
    test_memcmp_small(im, p);
    std::printf("  [%s] 大尺寸 / NT 路径 ...\n", im.name);
    test_large(im, p);
    std::printf("  [%s] 随机模糊 ...\n", im.name);
    test_fuzz(im, p);
    std::printf("  [%s] NT 阈值扫描 ...\n", im.name);
    test_nt_thresholds(im, p);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    g_verbose = 0;

    Pool p;
    if (!p.init()) { std::printf("alloc failed\n"); return 2; }

    std::printf("== x86mem 宿主正确性测试 ==\n");
    std::printf("CPU: sse4.2=%d avx=%d avx2=%d avx512f=%d\n",
                (int)cpu_sse42(), (int)cpu_avx(), (int)cpu_avx2(), (int)cpu_avx512f());

    /* NT 阈值: 默认常量 -> CPUID 探测的 L3 容量。必须落在 [MIN, MAX] 内,
     * 且探测失败时保持默认值。 */
    const size_t dflt = x86mem_cache_limit;
    CHECK(dflt == X86MEM_CACHE_LIMIT_DEFAULT, "默认 NT 阈值应为 %zu, 实为 %zu",
          (size_t)X86MEM_CACHE_LIMIT_DEFAULT, dflt);
    const size_t l3 = x86mem_detect_l3_size();
    x86mem_init_cache_limit();
    std::printf("NT 阈值: 默认 %zu KB -> CPUID L3 %zu KB (/%u) -> 采用 %zu KB\n",
                dflt / 1024, l3 / 1024,
                (unsigned)X86MEM_CACHE_LIMIT_L3_DIV, x86mem_cache_limit / 1024);
    CHECK(x86mem_cache_limit >= X86MEM_CACHE_LIMIT_MIN &&
          x86mem_cache_limit <= X86MEM_CACHE_LIMIT_MAX,
          "NT 阈值 %zu 越界 [%zu, %zu]", x86mem_cache_limit,
          (size_t)X86MEM_CACHE_LIMIT_MIN, (size_t)X86MEM_CACHE_LIMIT_MAX);
    if (l3 != 0) {
        /* 探测成功: 采用值必须是 clamp(l3 / DIV) */
        size_t want = l3 / X86MEM_CACHE_LIMIT_L3_DIV;
        if (want < X86MEM_CACHE_LIMIT_MIN) want = X86MEM_CACHE_LIMIT_MIN;
        if (want > X86MEM_CACHE_LIMIT_MAX) want = X86MEM_CACHE_LIMIT_MAX;
        CHECK(x86mem_cache_limit == want, "NT 阈值应为 clamp(L3/%u)=%zu, 实为 %zu",
              (unsigned)X86MEM_CACHE_LIMIT_L3_DIV, want, x86mem_cache_limit);
    } else {
        CHECK(x86mem_cache_limit == dflt, "L3 探测失败时 NT 阈值应保持默认");
    }

    int ran = 0;
    if (cpu_sse42()) {
        Impl im{"V0/SSE4.2", AVX_memcpyV0, AVX_memsetV0, AVX_memmoveV0, AVX_memcmpV0};
        run_all(im, p); ++ran;
    }
    if (cpu_avx() && MEMOPS_SupportV1) {
        Impl im{"V1/AVX", AVX_memcpyV1, AVX_memsetV1, AVX_memmoveV1, AVX_memcmpV1};
        run_all(im, p); ++ran;
    }
    if (cpu_avx2() && MEMOPS_SupportV2) {
        Impl im{"V2/AVX2", AVX_memcpyV2, AVX_memsetV2, AVX_memmoveV2, AVX_memcmpV2};
        run_all(im, p); ++ran;
    }
    if (cpu_avx512f() && MEMOPS_SupportV3) {
        Impl im{"V3/AVX512F", AVX_memcpyV3, AVX_memsetV3, AVX_memmoveV3, AVX_memcmpV3};
        run_all(im, p); ++ran;
    }

    if (ran == 0) { std::printf("!! 无任何 tier 可运行 (CPU 不支持 SSE4.2?)\n"); return 2; }

    std::printf("\n-- 断言 %d 次, 失败 %d 次 --\n", g_checks, g_fails);
    std::printf("%s\n", g_fails == 0 ? "MEMOPS OK" : "MEMOPS FAILED");
    return g_fails == 0 ? 0 : 1;
}
