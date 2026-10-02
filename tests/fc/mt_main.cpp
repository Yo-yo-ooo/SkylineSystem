// tests/fc/mt_main.cpp — 文件缓存多线程并发测试
// 4 宿主线程 = 4 虚拟 CPU, 各持一个 file_cache_cpu_t 实例;
// dirty promote 广播到其他线程的实例 → 真实跨实例并发 + 自旋锁竞争。
// 巡检: 锁住实例后 art_iter 键值一致性; 断言 0 失败 / 0 残留。
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <unistd.h>
#include <alloca.h>

#include <arch/x86_64/smp/smp.h>
#include <mem/heap.h>
#include <fs/fc.h>
#include "kernel_shim.h"
#undef __init
#undef max
#undef min
#undef swap
extern "C" void fc_track_enable(int on);
extern "C" void fc_free_log_enable(int on);

using namespace std::chrono;

static const int NT = 4;
static file_cache_cpu_t caches[NT];
static std::mutex g_mu;
static std::atomic<uint64_t> failures{0};
static std::atomic<int> watchdog_ok{1};

static thread_local uint64_t g_rng = 0xFC00D11 + 0x1000;
static uint64_t xrnd() { uint64_t x = g_rng; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_rng = x; }
static uint64_t rnd(uint64_t lo, uint64_t hi) { return lo + (xrnd() % (hi - lo + 1)); }

static int32_t wb_ok(uint64_t fid, const uint8_t *k, uint32_t kl, void *d, size_t dl) {
    (void)k; (void)kl; (void)d; (void)dl;
    return 0;
}

static void *kcopy(const char *data, size_t n) {
    void *p = kmalloc(n + 1);
    if (!p) return nullptr;
    memcpy(p, data, n);
    return p;
}

// 巡检: 锁住实例后校验 ART 键值一致性 (悬垂/双释在此暴露)
static thread_local art_tree *check_tree = nullptr;
static thread_local int check_cb_fail = 0;
static int32_t check_cb(void *data, const uint8_t *key, uint32_t key_len, void *value) {
    (void)data;
    file_cache_entry_t *e = (file_cache_entry_t *)value;
    if (e->key_len > 4096 || e->cpu_id >= 64 || e->pin_count > 1000000 ||
        art_search(check_tree, key, (int32_t)key_len) != value)
        check_cb_fail = 1;
    return 0;
}
static void integrity(int ci) {
    check_cb_fail = 0;
    check_tree = &caches[ci].index;
    spinlock_lock(&caches[ci].lock);
    art_iter(&caches[ci].index, check_cb, NULL);
    spinlock_unlock(&caches[ci].lock);
    if (check_cb_fail) failures++;
}

static void worker(int tid) {
    test_cpu_set_id((uint32_t)tid);
    file_cache_cpu_t *s = &caches[tid];
    for (uint64_t i = 0; i < 300000; i++) {
        std::string key = "f" + std::to_string(i % 200);   // 避开 kprintf.h 的 snprintf 宏
        uint64_t op = rnd(0, 100);
        if (op < 45) {
            size_t len = (size_t)rnd(1, 512);
            char data[512];                       // 定长数组: 每轮复用槽位,
            memset(data, (int)(tid + 1), len);    // alloca 会在循环内累积撑爆线程栈
            void *kbuf = kcopy(data, len);
            if (!kbuf) continue;
            bool dirty = (rnd(0, 99) < 30);   // 30% 脏 → 跨实例广播
            if (file_cache_promote(s, (const uint8_t*)key.data(), (uint32_t)key.size(), kbuf, len, dirty, 0, i) != 0)
                kfree(kbuf);
        } else if (op < 80) {
            size_t out = 0;
            file_cache_entry_t *e = nullptr;
            void *d = file_cache_get(s, (const uint8_t*)key.data(), (uint32_t)key.size(), 512, &out, &e);
            if (e) file_cache_put(s, e);
            (void)d;
        } else if (op < 90) {
            file_cache_invalidate(s, (const uint8_t*)key.data(), (uint32_t)key.size());
        } else {
            file_cache_idle_handler(s);
            file_cache_tick(s);
        }
        if ((i % 20000) == 0) { for (int c = 0; c < NT; c++) integrity(c); }
    }
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("FC MT TEST (4 线程 × 4 实例, 各 30 万 op, 30%% 脏写跨实例广播)\n");
    SLAB::Init();
    SLUB::InitKmalloc();
    fc_track_enable(0);     // 追踪器表无锁, MT 下保持关闭
    fc_free_log_enable(0);  // 裸跑: 排除日志并发副作用
    for (int i = 0; i < NT; i++)
        file_cache_cpu_init(&caches[i], (uint32_t)i, wb_ok);

    std::thread wd([&] {
        auto t0 = steady_clock::now();
        while (watchdog_ok.load()) {
            if (duration_cast<seconds>(steady_clock::now() - t0).count() > 90) {
                printf("  FAIL: 疑似死锁 (90s 看门狗)\n");
                _exit(3);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });

    std::vector<std::thread> ts;
    for (int i = 0; i < NT; i++) ts.emplace_back(worker, i);
    for (auto &t : ts) t.join();

    for (int c = 0; c < NT; c++) integrity(c);
    for (int c = 0; c < NT; c++) file_cache_cpu_destroy(&caches[c]);

    watchdog_ok = 0;
    wd.join();
    printf("FC MT RESULT: failures=%llu\n", (unsigned long long)failures.load());
    return failures.load() == 0 ? 0 : 1;
}
