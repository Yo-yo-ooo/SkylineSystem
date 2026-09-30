// tests/slub/main.cpp — SLUB/SLAB 内核堆 完整验证套件（宿主）
// 编译真实 kernel/src/mem/heap.cpp, 仅换 shim。
// 度量规范: 1 op = kmalloc+kfree 对(除非注明); 计时用批量法(两次 clock 夹 N ops);
// 预热 50k; 校验和 sink 防死码消除; 种子打印可重放。
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <chrono>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>

#include <mem/heap.h>
#include "kernel_shim.h"
#undef __init
#undef max
#undef min
#undef swap

using namespace std::chrono;

static const uint64_t SEED = 0x9E3779B97F4A7C15ULL;
static uint64_t g_rng_state = SEED;
static uint64_t xorshift64() { uint64_t x = g_rng_state; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_rng_state = x; }
static uint64_t rnd(uint64_t lo, uint64_t hi) { return lo + (xorshift64() % (hi - lo + 1)); }
static uint64_t failures = 0;

static inline uint64_t rdtsc() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static long long now_ms() { return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count(); }

// 批量计时: 预热 + N ops 两次 clock
template <typename F>
static double bench_ops(uint64_t N, F &&fn, uint64_t &sink) {
    for (uint64_t i = 0; i < 50000; i++) fn(sink);
    auto t0 = steady_clock::now();
    for (uint64_t i = 0; i < N; i++) fn(sink);
    auto t1 = steady_clock::now();
    double sec = duration_cast<nanoseconds>(t1 - t0).count() / 1e9;
    return N / sec;
}

static void check(bool cond, const char *what) {
    if (!cond) { failures++; printf("  FAIL: %s\n", what); }
}

struct LiveBlock { void *ptr; size_t size; uint8_t canary; };

static bool verify_canary(const LiveBlock &b) {
    uint8_t *p = (uint8_t*)b.ptr;
    for (size_t i = 0; i < b.size; i++) if (p[i] != b.canary) return false;
    return true;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("SLUB TEST SUITE (seed=0x%llx, g++ -O2, 单线程宿主)\n", (unsigned long long)SEED);
    kalloc_log_enable(1);
    SLAB::Init();
    SLUB::InitKmalloc();

    // ============ 1. 边界尺寸 + 对齐 + 唯一性 ============
    printf("\n== BOUNDARY SIZES & ALIGNMENT ==\n");
    {
        const size_t sizes[] = {0,1,15,16,17,31,32,63,64,127,128,255,256,511,512,1023,1024,1025,2047,2048,4095,4096,4097,8192,65536};
        uint64_t dup = 0, misalign = 0, nullc = 0;
        std::vector<uintptr_t> seen;
        std::vector<void*> keep;
        for (size_t s : sizes) {
            for (int k = 0; k < 32; k++) {
                void *p = kmalloc(s);
                if (!p) { nullc++; continue; }
                keep.push_back(p);
                if ((uintptr_t)p % alignof(max_align_t) != 0) misalign++;
                for (uintptr_t a : seen) if (a == (uintptr_t)p) { dup++; break; }
                seen.push_back((uintptr_t)p);
            }
        }
        printf("  25 种尺寸 × 32 次: null=%llu dup=%llu misalign=%llu\n",
               (unsigned long long)nullc, (unsigned long long)dup, (unsigned long long)misalign);
        check(nullc == 0, "全部尺寸应分配成功(含 kmalloc(0))");
        check(dup == 0, "无重复地址");
        check(misalign == 0, "全部满足 alignof(max_align_t)=16");
        for (void *p : keep) kfree(p);   // 释放本段测试块
    }

    // ============ 2. krealloc 语义 ============
    printf("\n== KREALLOC SEMANTICS ==\n");
    {
        void *p = kmalloc(128); memset(p, 0x3C, 128);
        void *q = krealloc(p, 64);                       // 缩容
        uint8_t pat64[64]; memset(pat64, 0x3C, 64);
        check(q != nullptr, "缩容非空");
        check(memcmp(q, pat64, 64) == 0, "缩容后内容保留");
        void *r = krealloc(q, 4096);                     // 跨类扩容
        check(r != nullptr, "扩容非空");
        check(memcmp(r, pat64, 64) == 0, "扩容后旧内容保留");
        memset(r, 0x5A, 4096);
        uint8_t pat8[8]; memset(pat8, 0x5A, 8);
        void *t = krealloc(r, 8);                        // 大幅缩容
        check(t != nullptr && memcmp(t, pat8, 8) == 0, "大幅缩容内容保留");
        void *z = krealloc(t, 0);                        // 缩到 0 = 释放
        check(z == nullptr, "krealloc(ptr,0) 返回 NULL");
        void *n = krealloc(nullptr, 64);                 // 等价 kmalloc
        check(n != nullptr, "krealloc(NULL,64) 非空");
        kfree(n);
        void *a = kmalloc(64); memset(a, 0x7E, 64);
        uint8_t pat64b[64]; memset(pat64b, 0x7E, 64);
        void *b = krealloc(a, 64);                       // 同尺寸
        check(b != nullptr && memcmp(b, pat64b, 64) == 0, "同尺寸内容保留");
        kfree(b);
    }

    // ============ 3. kcalloc 溢出 ============
    printf("\n== KCALLOC OVERFLOW ==\n");
    {
        void *p1 = kcalloc(1ULL << 40, 1ULL << 12);
        void *p2 = kcalloc(1ULL << 32, 1ULL << 32);
        void *p3 = kcalloc(1ULL << 63, 8);
        printf("  kcalloc 溢出: %p %p %p (均应为 NULL)\n", p1, p2, p3);
        check(p1 == nullptr && p2 == nullptr && p3 == nullptr, "乘法回绕必须拒绝");
        void *ok = kcalloc(16, 64);
        check(ok != nullptr, "正常 kcalloc 非空");
        kfree(ok);
    }

    // ============ 4. OOM 故障注入 ============
    printf("\n== OOM FAULT INJECTION ==\n");
    {
        void *p = kmalloc(64); memset(p, 0x69, 64);
        uint8_t pat69[64]; memset(pat69, 0x69, 64);
        kmem_cache *kc = SLUB::Create("oom_test", 64, 8);   // 先建缓存(需要页)
        vmm_set_fail_all(1);
        void *big = kmalloc(8192);
        check(big == nullptr, "页分配失败时 kmalloc(8192) 返回 NULL");
        void *r = krealloc(p, 16384);
        check(r == nullptr, "OOM 时 krealloc 返回 NULL");
        check(memcmp(p, pat69, 64) == 0, "OOM 时旧块内容保留");
        void *o = kc ? SLUB::Alloc(kc) : nullptr;
        check(kc != nullptr && o == nullptr, "冷缓存 OOM 时 SLUB::Alloc 返回 NULL");
        vmm_set_fail_all(0);
        check(kmalloc(8192) != nullptr, "故障解除后恢复分配");
        if (kc) { void *o2 = SLUB::Alloc(kc); check(o2 != nullptr, "恢复后冷缓存可分配"); if (o2) SLUB::Free(kc, o2); SLUB::Destroy(kc); }
        kfree(p);
    }

    // ============ 5. 混沌(两阶段) ============
    printf("\n== CHAOS (2 阶段 × 200 万 op) ==\n");
    std::vector<LiveBlock> live;
    uint64_t checks = 0;
    for (int phase = 1; phase <= 2; phase++) {
        for (uint64_t iter = 0; iter < 2000000; iter++) {
            uint64_t op = rnd(0, 100);
            if (op < 45 && live.size() < 40000) {
                size_t size = (size_t)rnd(1, 4096);
                uint8_t canary = (uint8_t)rnd(1, 255);
                void *p = kmalloc(size);
                if (!p) { failures++; continue; }
                memset(p, canary, size);
                live.push_back({p, size, canary});
            } else if (op < 80 && !live.empty()) {
                size_t idx = (size_t)rnd(0, live.size() - 1);
                LiveBlock b = live[idx]; live[idx] = live.back(); live.pop_back();
                if (!verify_canary(b)) { failures++; printf("  CANARY CORRUPTION op=%llu size=%zu\n", (unsigned long long)iter, b.size); }
                kfree(b.ptr);
            } else if (op < 90 && !live.empty()) {
                size_t idx = (size_t)rnd(0, live.size() - 1);
                LiveBlock b = live[idx];
                size_t ns = (size_t)rnd(1, 8192);
                void *np = krealloc(b.ptr, ns);
                if (np) {
                    size_t keep = b.size < ns ? b.size : ns;
                    bool ok = true;
                    for (size_t i = 0; i < keep; i++) if (((uint8_t*)np)[i] != b.canary) { ok = false; break; }
                    if (!ok) { failures++; printf("  REALLOC 内容丢失 op=%llu\n", (unsigned long long)iter); }
                    memset(np, b.canary, ns);
                    live[idx] = {np, ns, b.canary};
                }
            } else if (!live.empty()) {
                checks++;
                size_t idx = (size_t)rnd(0, live.size() - 1);
                if (!verify_canary(live[idx])) { failures++; printf("  CANARY CORRUPTION op=%llu\n", (unsigned long long)iter); }
            }
        }
        for (auto &b : live) { if (!verify_canary(b)) failures++; kfree(b.ptr); }
        live.clear();
        printf("  phase %d: spot_checks=%llu failures=%llu live_pages=%llu\n",
               phase, (unsigned long long)checks, (unsigned long long)failures,
               (unsigned long long)VMM::LivePages());
    }
    // 碎片化: 混沌后重填 50k 存活块, 统计内部碎片
    printf("\n== FRAGMENTATION ==\n");
    {
        uint64_t req_sum = 0, cls_sum = 0;
        for (int i = 0; i < 50000; i++) {
            size_t s = (size_t)rnd(1, 2048);
            void *p = kmalloc(s);
            if (p) {
                size_t cls = SLUB::TryGetSize(p);
                if (cls == 0) cls = SLAB::GetSize(p);
                req_sum += s; cls_sum += cls;
                live.push_back({p, s, 0x11});
            }
        }
        for (auto &b : live) kfree(b.ptr);
        live.clear();
        printf("  内部碎片率: (类大小-请求)/请求 = %.1f%% (req=%lluB cls=%lluB)\n",
               req_sum ? 100.0 * (double)(cls_sum - req_sum) / (double)req_sum : 0.0,
               (unsigned long long)req_sum, (unsigned long long)cls_sum);
        dump_live_pages();   // live_pages 构成机制
    }

    // 碎片化趋势: 10 轮 (混沌 50k op → 记页数) + 每轮 largest_free_span
    printf("\n== FRAGMENTATION TREND (10 轮 × 5 万 op 混沌, 页数与最大连续可用) ==\n");
    {
        uint64_t pages_prev = 0, pages_now = 0;
        for (int round = 0; round < 10; round++) {
            std::vector<LiveBlock> keep;
            for (int i = 0; i < 50000; i++) {
                uint64_t op = rnd(0, 99);
                if (op < 45 && keep.size() < 4000) {
                    size_t s = (size_t)rnd(1, 2048);
                    void *p = kmalloc(s);
                    if (p) keep.push_back({p, s, 0x33});
                } else if (!keep.empty()) {
                    size_t idx = (size_t)rnd(0, keep.size() - 1);
                    kfree(keep[idx].ptr);
                    keep[idx] = keep.back();
                    keep.pop_back();
                } else {
                    void *p = kmalloc((size_t)rnd(1, 2048));
                    if (p) kfree(p);
                }
            }
            for (auto &b : keep) kfree(b.ptr);
            pages_now = VMM::LivePages();
            /* largest_free_span: 逐级增大单块请求直至 NULL (未标定页压力下) */
            size_t span = 0;
            for (size_t try_sz = 64 * 1024; try_sz <= 4 * 1024 * 1024; try_sz *= 2) {
                void *big = kmalloc(try_sz);
                if (big) { span = try_sz; kfree(big); } else break;
            }
            printf("  round %d: pages=%llu%s largest_free>=%zuB\n", round,
                   (unsigned long long)pages_now,
                   round == 0 ? "" : (pages_now == pages_prev ? " (与上轮持平)" : " (Δ)"),
                   span);
            pages_prev = pages_now;
        }
    }

    // ============ 6. 负向用例(必须失败/防护) ============
    printf("\n== NEGATIVE TESTS (fork, 期望 SIGABRT) ==\n");
    {
        pid_t pid = fork();
        if (pid == 0) { void *d = kmalloc(64); kfree(d); kfree(d); _exit(0); }
        int st = 0; waitpid(pid, &st, 0);
        bool guarded = WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
        printf("  double-free guard: %s\n", guarded ? "ACTIVE" : "MISSING");
        check(guarded, "双重释放必须被拒绝(SIGABRT)");
    }

    // ============ 7. 延迟分布 ============
    printf("\n== LATENCY DISTRIBUTION (128B 类, 200 万 op) ==\n");
    {
        // tsc 频率标定
        auto c0 = steady_clock::now(); uint64_t r0 = rdtsc();
        while (duration_cast<milliseconds>(steady_clock::now() - c0).count() < 200) {}
        auto c1 = steady_clock::now(); uint64_t r1 = rdtsc();
        double ghz = (double)(r1 - r0) / 1e9 /
                     (double)duration_cast<nanoseconds>(c1 - c0).count() * 1e9;
        std::vector<uint64_t> lat;
        lat.reserve(2000000);
        for (int i = 0; i < 50000; i++) { void *p = kmalloc(128); kfree(p); }
        for (int i = 0; i < 2000000; i++) {
            uint64_t a = rdtsc();
            void *p = kmalloc(128);
            kfree(p);
            lat.push_back(rdtsc() - a);
        }
        std::sort(lat.begin(), lat.end());
        auto ns = [&](uint64_t t) { return t / ghz; };
        printf("  无压力: P50=%.0fns P95=%.0fns P99=%.0fns max=%.0fns (tsc=%.2fGHz)\n",
               ns(lat[1000000]), ns(lat[1900000]), ns(lat[1980000]), ns(lat.back()), ghz);
        // 高负载: 保持 100k 存活块再测
        std::vector<void*> hold;
        for (int i = 0; i < 100000; i++) hold.push_back(kmalloc(256));
        lat.clear();
        for (int i = 0; i < 1000000; i++) {
            uint64_t a = rdtsc();
            void *p = kmalloc(128);
            kfree(p);
            lat.push_back(rdtsc() - a);
        }
        std::sort(lat.begin(), lat.end());
        printf("  高负载(10 万存活 256B 块): P50=%.0fns P99=%.0fns max=%.0fns\n",
               ns(lat[500000]), ns(lat[990000]), ns(lat.back()));
        for (void *p : hold) kfree(p);

        // 第二轮: 验证 max 是否复现 (首触缺页/页分配应只出现在第一轮)
        kmem_cache *k128 = SLUB::KmallocCacheFor(128);
        uint64_t slow_before = k128 ? SLUB::SlowCount(k128) : 0;
        uint64_t refill_before = k128 ? SLUB::RefillCount(k128) : 0;
        lat.clear();
        for (int i = 0; i < 2000000; i++) {
            uint64_t a = rdtsc();
            void *p = kmalloc(128);
            kfree(p);
            lat.push_back(rdtsc() - a);
        }
        std::sort(lat.begin(), lat.end());
        printf("  第二轮(热缓存, 验 max 复现): P50=%.0fns P99=%.0fns max=%.0fns\n",
               ns(lat[1000000]), ns(lat[1980000]), ns(lat.back()));
        // 离群归因: 统计本轮慢路径次数 (slow_alloc) 与 >10µs 离群数
        uint64_t slow_n = k128 ? SLUB::SlowCount(k128) - slow_before : 0;
        uint64_t refill_n = k128 ? SLUB::RefillCount(k128) - refill_before : 0;
        uint64_t outliers = 0;
        for (uint64_t t : lat) if (ns(t) > 10000) outliers++;
        printf("  归因: 本轮慢路径 %llu 次(partial 补位 %llu, 新建 slab %llu), >10µs 离群 %llu 个 (2M op)\n",
               (unsigned long long)slow_n, (unsigned long long)refill_n,
               (unsigned long long)(slow_n > refill_n ? slow_n - refill_n : 0),
               (unsigned long long)outliers);
    }

    // ============ 8. 尺寸分档吞吐 ============
    printf("\n== SIZE-CLASS THROUGHPUT (op = kmalloc+kfree 对, 批量计时, 预热 50k) ==\n");
    {
        uint64_t sink = 0;
        const size_t classes[] = {16, 32, 64, 128, 256, 512, 1024};
        for (size_t sz : classes) {
            double best = 0, worst = 1e18;
            for (int run = 0; run < 3; run++) {
                double ops_s = bench_ops(2000000, [&](uint64_t &s) {
                    void *p = kmalloc(sz);
                    if (p) { s += ((uint8_t*)p)[0]; kfree(p); }
                }, sink);
                best = std::max(best, ops_s); worst = std::min(worst, ops_s);
            }
            printf("  kmalloc(%zuB): %.2f M ops/s (3 run 区间 %.2f-%.2f)\n",
                   sz, best / 1e6, worst / 1e6, best / 1e6);
        }
        // krealloc 混合
        double kro = bench_ops(2000000, [&](uint64_t &s) {
            void *p = kmalloc(64);
            if (p) { void *q = krealloc(p, 128); if (q) { s += ((uint8_t*)q)[0]; kfree(q); } else kfree(p); }
        }, sink);
        printf("  krealloc 64->128: %.2f M ops/s\n", kro / 1e6);
        printf("  (sink=0x%llx 防死码消除)\n", (unsigned long long)sink);
    }

    printf("\nSLUB RESULT: failures=%llu\n", (unsigned long long)failures);
    return failures == 0 ? 0 : 1;
}
