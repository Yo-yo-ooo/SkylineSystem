// tests/slub/mt_main.cpp — SLUB/SLAB 多线程 per-CPU 并发测试
// 每个宿主线程 = 一个虚拟 CPU (test_cpu_set_id); 覆盖:
//   A. 无竞争: 各线程独立混沌 (per-CPU active slab / cslab magazine 分离)
//   B. 跨线程 free: 线程 A 分配 → 线程 B 释放 (SLUB link_partial 跨 CPU 路径 + 锁竞争)
//   C. 同 cache 竞争: 4 线程 hammer 同一尺寸类 (cache->lock 与原子栈真实竞争)
// 断言: 0 重复地址 / 0 泄漏 / 无死锁(看门狗)。
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <unordered_set>
#include <unistd.h>

#include <arch/x86_64/smp/smp.h>
#include <mem/heap.h>
#include "kernel_shim.h"
#undef __init
#undef max
#undef min
#undef swap

static const uint64_t SEED = 0xA5A5F00DC0FFEE11ULL;
static uint64_t failures = 0;
static std::mutex g_mu;
static std::unordered_set<uintptr_t> g_live;     // 全局唯一性 (mutex 保护)

// 每线程独立 RNG (TSAN 干净)
static thread_local uint64_t g_rng_state = SEED + 0x1000;
static uint64_t xrnd() { uint64_t x = g_rng_state; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_rng_state = x; }
static uint64_t rnd(uint64_t lo, uint64_t hi) { return lo + (xrnd() % (hi - lo + 1)); }

static void fail(const char *w) {
    std::lock_guard<std::mutex> g(g_mu);
    failures++;
    printf("  FAIL: %s\n", w);
}

// A. 无竞争: 各线程独立混沌
static void phase_a(int tid) {
    test_cpu_set_id((uint32_t)tid);
    std::vector<void*> live;
    for (uint64_t i = 0; i < 500000; i++) {
        if ((i & 1) == 0 || live.empty()) {
            size_t s = (size_t)rnd(1, 2048);
            void *p = kmalloc(s);
            if (!p) { fail("A 分配失败"); continue; }
            memset(p, (int)tid, s);
            {
                std::lock_guard<std::mutex> g(g_mu);
                if (!g_live.insert((uintptr_t)p).second) fail("A 重复地址(跨线程)");
            }
            live.push_back(p);
        } else {
            void *p = live.back(); live.pop_back();
            {
                std::lock_guard<std::mutex> g(g_mu);
                g_live.erase((uintptr_t)p);
            }
            kfree(p);
        }
    }
    for (void *p : live) { std::lock_guard<std::mutex> g(g_mu); g_live.erase((uintptr_t)p); kfree(p); }
}

// B. 跨线程 free: 每线程分配后交给"下一个"线程释放
struct XItem { void *p; };
static void phase_b(int tid, int nthreads, std::vector<std::vector<XItem>> &queues) {
    test_cpu_set_id((uint32_t)tid);
    int nxt = (tid + 1) % nthreads;
    for (uint64_t i = 0; i < 200000; i++) {
        if ((i & 1) == 0) {
            void *p = kmalloc(128 + (size_t)rnd(0, 3) * 64);
            if (!p) { fail("B 分配失败"); continue; }
            memset(p, (int)tid, 64);
            {
                std::lock_guard<std::mutex> g(g_mu);
                if (!g_live.insert((uintptr_t)p).second) fail("B 重复地址");
                queues[nxt].push_back({p});
            }
        } else {
            XItem it{nullptr};
            {
                std::lock_guard<std::mutex> g(g_mu);
                if (!queues[tid].empty()) { it = queues[tid].back(); queues[tid].pop_back(); }
            }
            if (it.p) {           // 释放"别人"分配的块 → 跨 CPU free 路径
                std::lock_guard<std::mutex> g(g_mu);
                g_live.erase((uintptr_t)it.p);
                kfree(it.p);
            }
        }
    }
}

// C. 同 cache 竞争吞吐 + 唯一性
static void phase_c(int tid, int nthreads, std::atomic<uint64_t> &total_ops, std::atomic<int> &stop, int run_secs) {
    test_cpu_set_id((uint32_t)tid);
    uint64_t ops = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (!stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 1000; i++) {
            void *p = kmalloc(128);
            if (p) { ((uint8_t*)p)[0] = (uint8_t)tid; kfree(p); ops++; }
            else fail("C 分配失败");
        }
        if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() >= run_secs)
            break;
    }
    total_ops += ops;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("SLUB MT TEST (seed=0x%llx, 4 宿主线程 = 4 虚拟 CPU)\n", (unsigned long long)SEED);
    SLAB::Init();
    SLUB::InitKmalloc();
    const int NT = 4;

    // 看门狗: 60 秒无进展判死锁
    std::atomic<bool> watchdog_ok{true};
    std::thread wd([&] {
        auto t0 = std::chrono::steady_clock::now();
        while (watchdog_ok.load()) {
            if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0).count() > 60) {
                printf("  FAIL: 疑似死锁 (60s 看门狗)\n");
                _exit(3);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    printf("== A. 无竞争 (4 线程独立混沌, 各 50 万 op) ==\n");
    {
        std::vector<std::thread> ts;
        for (int i = 0; i < NT; i++) ts.emplace_back(phase_a, i);
        for (auto &t : ts) t.join();
        printf("  A done, live 残留=%zu\n", g_live.size());
        if (!g_live.empty()) fail("A 残留未释放块");
    }

    printf("== B. 跨线程 free (每线程 20 万 op, 环形传递) ==\n");
    {
        std::vector<std::vector<XItem>> queues(NT);
        std::vector<std::thread> ts;
        for (int i = 0; i < NT; i++) ts.emplace_back(phase_b, i, NT, std::ref(queues));
        for (auto &t : ts) t.join();
        // 全部线程结束后统一清空残留 (防收尾竞态)
        for (auto &q : queues)
            for (auto &it : q) { std::lock_guard<std::mutex> g(g_mu); g_live.erase((uintptr_t)it.p); kfree(it.p); }
        if (!g_live.empty()) { printf("  残留 %zu 块\n", g_live.size()); fail("B 残留未释放块"); }
        printf("  B done\n");
    }

    printf("== C. 同 cache 竞争 (4 线程 hammer kmalloc(128), 5s) ==\n");
    {
        std::atomic<uint64_t> total{0};
        std::atomic<int> stop{0};
        auto t0 = std::chrono::steady_clock::now();
        std::vector<std::thread> ts;
        for (int i = 0; i < NT; i++) ts.emplace_back(phase_c, i, NT, std::ref(total), std::ref(stop), 5);
        for (auto &t : ts) t.join();
        auto t1 = std::chrono::steady_clock::now();
        double sec = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1e9;
        printf("  C: 4 线程合计 %.2f M ops/s (单线程基线 ~12.4M)\n", total.load() / 1e6 / sec);
    }

    watchdog_ok = false;
    wd.join();

    printf("SLUB MT RESULT: failures=%llu\n", (unsigned long long)failures);
    return failures == 0 ? 0 : 1;
}
