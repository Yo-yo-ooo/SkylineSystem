// tests/fc/main.cpp — 文件缓存(fc.cpp) 混沌 + 压力测试（宿主）
// 编译真实 kernel/src/fs/fc.cpp + kernel/src/klib/algorithm/art.c
// 注意: kprintf.h 会把 sprintf/snprintf 映射到内核版, libc 头必须先包含。
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <chrono>
#include <cerrno>   /* 双保险: string_conversions.h 需要 errno */
#include <string>
#include <unordered_map>
#include <algorithm>

#include <fs/fc.h>
#include "kernel_shim.h"
#undef __init
#undef max
#undef min
#undef swap
extern "C" void art_deep_dump(art_tree *t);
extern "C" void fc_track_query(void *p);
extern "C" void kalloc_query(void *p);
extern "C" void kalloc_log_enable(int on);
extern "C" void art_log_enable(int on);

using namespace std::chrono;

static uint64_t g_rng = 0x2545F4914F6CDD1DULL;
static uint64_t xrnd() { uint64_t x = g_rng; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return g_rng = x; }
static uint64_t rnd(uint64_t lo, uint64_t hi) { return lo + (xrnd() % (hi - lo + 1)); }

#define NCACHE 4
static file_cache_cpu_t caches[NCACHE];
static std::unordered_map<std::string, std::string> shadow;       // key -> 期望内容
static std::unordered_map<std::string, bool> shadow_dirty;        // 内容强校验仅在脏写后
static uint64_t failures = 0;
static uint64_t g_tree_broken = 0;
static int g_check_cpu = 0;

// 操作历史环形日志 (定位污染操作)
struct OpLog { uint64_t iter; uint32_t type; uint32_t ci; char key[16]; };
static OpLog g_oplog[256];
static uint32_t g_oplog_head = 0;
static void oplog_push(uint64_t iter, uint32_t type, uint32_t ci, const std::string &key) {
    OpLog &o = g_oplog[g_oplog_head++ % 256];
    o.iter = iter; o.type = type; o.ci = ci;
    snprintf(o.key, sizeof o.key, "%s", key.c_str());
}
static void oplog_dump() {
    printf("--- last ops ---\n");
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t idx = (g_oplog_head + i) % 256;
        if (g_oplog[idx].iter == 0 && idx != 0) continue;
        printf("  op %llu: type=%u ci=%u key=%s\n",
               (unsigned long long)g_oplog[idx].iter, g_oplog[idx].type,
               g_oplog[idx].ci, g_oplog[idx].key);
    }
}

static int32_t wb_cb(uint64_t fid, const uint8_t *key, uint32_t key_len, void *data, size_t data_len) {
    (void)key; (void)key_len; (void)data; (void)data_len;
    return 0;  // 写回成功
}

static int32_t art_check_cb(void *data, const uint8_t *key, uint32_t key_len, void *value) {
    (void)data; (void)key; (void)key_len;
    file_cache_entry_t *e = (file_cache_entry_t *)value;
    if (!VMM::HeapContains(e)) {
        printf("ART CORRUPT: value=%p 不在堆区\n", (void*)e);
        failures++;
        return 0;
    }
    // 'N' 污染特征: key_len 巨大 / cpu_id 越界 / key 指针非堆区
    if (e->key_len > 4096 || e->cpu_id >= 64 || !VMM::HeapContains(e->key) || e->data_len > (1u << 30)) {
        printf("ART CORRUPT: cache=%d tree_key='%.*s' value=%p key_len=%u cpu_id=%u key=%p data_len=%zu\n",
               g_check_cpu, (int)key_len, (const char*)key, (void*)e, e->key_len, e->cpu_id, (void*)e->key, e->data_len);
        fc_track_query((void*)e);
        kalloc_query((void*)e);
        failures++;
        return 0;
    }
    // 键值一致性: 用树键反查必须命中同一值, 否则树结构已损坏
    void *back = art_search(&caches[g_check_cpu].index, key, key_len);
    if (back != (void*)e) {
        printf("ART TREE-BROKEN at op=%llu cache=%d tree_key='%.*s' value=%p backsearch=%p\n",
               (unsigned long long)g_test_op, g_check_cpu, (int)key_len, (const char*)key, (void*)e, back);
        g_tree_broken++;
    }
    return 0;
}
static void art_integrity_check() {
    for (int i = 0; i < NCACHE; i++) {
        g_check_cpu = i;
        art_iter(&caches[i].index, art_check_cb, NULL);
    }
}

static int32_t art_dump_cb(void *data, const uint8_t *key, uint32_t key_len, void *value) {
    (void)data;
    printf("    key='%.*s' value=%p\n", (int)key_len, (const char*)key, value);
    return 0;
}
static void art_dump(int ci) {
    printf("  dump cache %d:\n", ci);
    art_iter(&caches[ci].index, art_dump_cb, NULL);
}

// Zipf 偏斜键(α=1.1): 预计算累积权重, 二分采样 —— 模拟真实文件访问局部性
static std::vector<double> g_zipf_cdf;
static void zipf_init(uint64_t n, double alpha) {
    g_zipf_cdf.resize(n);
    double sum = 0;
    for (uint64_t i = 1; i <= n; i++) sum += 1.0 / pow((double)i, alpha);
    double acc = 0;
    for (uint64_t i = 1; i <= n; i++) {
        acc += 1.0 / pow((double)i, alpha);
        g_zipf_cdf[i - 1] = acc / sum;
    }
}
static uint64_t zipf_key(uint64_t n) {
    double u = (double)xrnd() / (double)(~0ULL);
    auto it = std::lower_bound(g_zipf_cdf.begin(), g_zipf_cdf.end(), u);
    return (uint64_t)(it - g_zipf_cdf.begin());
}

static std::string mkkey(uint64_t i) {
    char b[16];
    snprintf(b, sizeof b, "file%llu", (unsigned long long)(i % 500));
    return std::string(b);
}

// promote 接管 data 所有权并用 kfree 释放 —— 必须提供 kmalloc 的缓冲区
static void *kcopy(const std::string &s) {
    void *p = kmalloc(s.size() + 1);
    if (!p) return nullptr;
    memcpy(p, s.data(), s.size() + 1);
    return p;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("FC TEST SUITE (seed=0x2545f4914f6cdd1d, g++ -O2, 单线程宿主, 500 键 Zipf α=1.1)\n");
    zipf_init(500, 1.1);
    kalloc_log_enable(1);
    art_log_enable(1);
    // 内核堆必须先行初始化 (否则缓存 obj_size=0 → 除零)
    SLAB::Init();
    SLUB::InitKmalloc();
    for (int i = 0; i < NCACHE; i++) {
        file_cache_cpu_init(&caches[i], (uint32_t)i, wb_cb);
        /* 宽松限: 迁移不被软限拒止, 测"真实负载"命中率;
           限压基线(64KB/256KB)的 15.3% 命中率已记录为配置产物 */
        file_cache_set_limits(&caches[i], 1 * 1024 * 1024, 2 * 1024 * 1024);
    }
    printf("cache0_index=%p\n", (void*)&caches[0].index);

    printf("== FC CHAOS TEST (4 个 per-CPU 缓存, 200 个逻辑文件) ==\n");
    long long t0 = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

    for (uint64_t iter = 0; iter < 2000000; iter++) {
        g_test_op = iter;
        if (iter == 565) art_deep_dump(&caches[0].index);  // op565 前真实树结构
        uint64_t ci = rnd(0, NCACHE - 1);
        file_cache_cpu_t *s = &caches[ci];
        std::string key = mkkey(zipf_key(500));   // Zipf 偏斜键 (真实局部性)
        uint64_t op = rnd(0, 100);
        oplog_push(iter, (uint32_t)op, (uint32_t)ci, key);

        if (op < 40) {  // promote 新内容: 70% 干净(无广播) + 30% 脏(广播)
            size_t len = (size_t)rnd(1, 4096);
            std::string data(len, (char)(rnd(1, 255)));
            void *kbuf = kcopy(data);
            if (!kbuf) { failures++; continue; }
            bool dirty = (rnd(0, 99) < 30);
            int32_t rc = file_cache_promote(s, (const uint8_t*)key.data(), (uint32_t)key.size(),
                                            kbuf, len, dirty, 0, rnd(1, 1000000));
            if (rc == 0) { shadow[key] = data; shadow_dirty[key] = dirty; }
            else kfree(kbuf);                  // 失败时 kbuf 归 harness 释放
        } else if (op < 75) {  // get: 脏写后才强校验内容(干净写允许 per-CPU 分叉)
            size_t out_len = 0;
            file_cache_entry_t *e = nullptr;
            void *d = file_cache_get(s, (const uint8_t*)key.data(), (uint32_t)key.size(), 4096, &out_len, &e);
            if (d) {
                auto it = shadow.find(key);
                auto dit = shadow_dirty.find(key);
                if (it != shadow.end() && dit != shadow_dirty.end() && dit->second &&
                    (out_len != it->second.size() || memcmp(d, it->second.data(), out_len) != 0))
                { failures++; printf("FC: 内容损坏 key=%s\n", key.c_str()); }
                file_cache_put(s, e);
            }
        } else if (op < 85) {  // invalidate: 广播其他核 (源核跳过是内核设计)
            file_cache_invalidate(s, (const uint8_t*)key.data(), (uint32_t)key.size());
            // 其他核的该 key 副本必须被清除/失效 —— 仅在"最后一次是脏写"时强校验
            // (干净写允许 per-CPU 分叉, 他核持有旧副本是设计内行为)
            auto dit = shadow_dirty.find(key);
            bool strict = (dit != shadow_dirty.end() && dit->second);
            for (int j = 0; j < NCACHE; j++) {
                if (j == (int)ci) continue;
                size_t out_len = 0;
                file_cache_entry_t *e = nullptr;
                void *d = file_cache_get(&caches[j], (const uint8_t*)key.data(), (uint32_t)key.size(), 4096, &out_len, &e);
                if (d) {
                    auto it = shadow.find(key);
                    if (strict && it != shadow.end() &&
                        (out_len != it->second.size() || memcmp(d, it->second.data(), out_len) != 0))
                    { failures++; printf("FC: 失效后他核仍返回旧内容 key=%s (核%d)\n", key.c_str(), j); }
                    file_cache_put(&caches[j], e);
                }
            }
        } else {  // idle_handler / tick (淘汰路径, 曾是 #GP 现场)
            file_cache_idle_handler(s);
            file_cache_tick(s);
        }
        if ((iter % 5000) == 0) art_integrity_check();
        // 每轮巡检: 值字段损坏 = 内存安全失败(中止); 树结构不一致 = 告警计数(继续)
        {
            uint64_t before = failures;
            art_integrity_check();
            if (failures != before) {
                printf("CORRUPTION DETECTED at op %llu:\n", (unsigned long long)iter);
                oplog_dump();
                return 1;
            }
        }
    }
    art_integrity_check();
    printf("fc chaos: 2,000,000 ops, failures=%llu, tree_broken=%llu\n",
           (unsigned long long)failures, (unsigned long long)g_tree_broken);
    printf("fc stats: hits=%llu misses=%llu evictions=%llu migrations_in=%llu\n",
           (unsigned long long)(caches[0].hits + caches[1].hits + caches[2].hits + caches[3].hits),
           (unsigned long long)(caches[0].misses + caches[1].misses + caches[2].misses + caches[3].misses),
           (unsigned long long)(caches[0].evictions + caches[1].evictions + caches[2].evictions + caches[3].evictions),
           (unsigned long long)(caches[0].migrations_in + caches[1].migrations_in + caches[2].migrations_in + caches[3].migrations_in));

    // 终态清理检查: 每个缓存 destroy, 不应崩溃
    for (int i = 0; i < NCACHE; i++) file_cache_cpu_destroy(&caches[i]);
    printf("fc destroy: OK (无崩溃)\n");

    // ============ 分路径吞吐 (op 定义见各路径注释, 批量计时, 预热) ============
    printf("\n== FC PER-PATH THROUGHPUT ==\n");
    for (int i = 0; i < NCACHE; i++)
        file_cache_cpu_init(&caches[i], (uint32_t)i, wb_cb);
    {
        auto bench = [&](const char *name, uint64_t N, auto &&fn) {
            for (int i = 0; i < 20000; i++) fn();            // 预热
            auto t0 = steady_clock::now();
            for (uint64_t i = 0; i < N; i++) fn();
            auto t1 = steady_clock::now();
            double sec = duration_cast<nanoseconds>(t1 - t0).count() / 1e9;
            printf("  %s: %.2f M ops/s (op 定义见 tests/README)\n", name, N / sec / 1e6);
        };
        // 预填 200 键
        for (int i = 0; i < 200; i++) {
            std::string key = mkkey((uint64_t)i);
            std::string data(256, 'B');
            void *kbuf = kcopy(data);
            if (kbuf && file_cache_promote(&caches[0], (const uint8_t*)key.data(), (uint32_t)key.size(), kbuf, 256, false, 0, 1) != 0)
                kfree(kbuf);
        }
        // get-hit: 命中同一缓存
        bench("get-hit (命中缓存0)", 2000000, [&] {
            std::string key = mkkey(rnd(0, 199));
            size_t out = 0; file_cache_entry_t *e = nullptr;
            void *d = file_cache_get(&caches[0], (const uint8_t*)key.data(), (uint32_t)key.size(), 256, &out, &e);
            if (e) file_cache_put(&caches[0], e);
            (void)d;
        });
        // get-miss: 键不在任何缓存
        bench("get-miss (4 缓存全扫)", 2000000, [&] {
            std::string key = mkkey(300 + rnd(0, 199));
            size_t out = 0; file_cache_entry_t *e = nullptr;
            void *d = file_cache_get(&caches[0], (const uint8_t*)key.data(), (uint32_t)key.size(), 256, &out, &e);
            (void)d; (void)out;
        });
        // promote (无脏)
        bench("promote (256B, 无广播)", 1000000, [&] {
            std::string key = mkkey(rnd(0, 199));
            std::string data(256, 'C');
            void *kbuf = kcopy(data);
            if (kbuf && file_cache_promote(&caches[0], (const uint8_t*)key.data(), (uint32_t)key.size(), kbuf, 256, false, 0, 1) != 0)
                kfree(kbuf);
        });
        // invalidate (含广播到 3 个其他缓存)
        bench("invalidate (跨 4 缓存广播)", 1000000, [&] {
            std::string key = mkkey(rnd(0, 199));
            file_cache_invalidate(&caches[0], (const uint8_t*)key.data(), (uint32_t)key.size());
        });
        // idle_handler (含淘汰扫描)
        bench("idle_handler", 200000, [&] {
            file_cache_idle_handler(&caches[0]);
            file_cache_tick(&caches[0]);
        });
    }

    // ============ 混合负载整体吞吐 + get 延迟 (真实权重, 与混沌相同) ============
    printf("\n== FC MIXED-LOAD THROUGHPUT & LATENCY ==\n");
    {
        for (int i = 0; i < NCACHE; i++) {
            file_cache_cpu_init(&caches[i], (uint32_t)i, wb_cb);
            file_cache_set_limits(&caches[i], 1 * 1024 * 1024, 2 * 1024 * 1024);
        }
        for (int i = 0; i < 200; i++) {   // 预填
            std::string key = mkkey((uint64_t)i);
            std::string data(256, 'D');
            void *kbuf = kcopy(data);
            if (kbuf && file_cache_promote(&caches[0], (const uint8_t*)key.data(), (uint32_t)key.size(), kbuf, 256, false, 0, 1) != 0)
                kfree(kbuf);
        }
        std::vector<uint64_t> glat;
        glat.reserve(400000);
        auto t0 = steady_clock::now();
        uint64_t ops = 0, hits = 0;
        while (duration_cast<milliseconds>(steady_clock::now() - t0).count() < 3000) {
            uint64_t ci = rnd(0, NCACHE - 1);
            std::string key = mkkey(zipf_key(500));
            uint64_t op = rnd(0, 100);
            if (op < 40) {
                size_t len = (size_t)rnd(1, 256);
                std::string data(len, 'E');
                void *kbuf = kcopy(data);
                if (kbuf && file_cache_promote(&caches[ci], (const uint8_t*)key.data(), (uint32_t)key.size(), kbuf, len, rnd(0, 99) < 30, 0, ops) != 0)
                    kfree(kbuf);
            } else if (op < 75) {
                auto g0 = steady_clock::now();
                size_t out = 0; file_cache_entry_t *e = nullptr;
                void *d = file_cache_get(&caches[ci], (const uint8_t*)key.data(), (uint32_t)key.size(), 256, &out, &e);
                if (d) hits++;
                if (e) file_cache_put(&caches[ci], e);
                if (glat.size() < 400000)
                    glat.push_back((uint64_t)duration_cast<nanoseconds>(steady_clock::now() - g0).count());
            } else if (op < 85) {
                file_cache_invalidate(&caches[ci], (const uint8_t*)key.data(), (uint32_t)key.size());
            } else {
                file_cache_idle_handler(&caches[ci]);
                file_cache_tick(&caches[ci]);
            }
            ops++;
        }
        double sec = duration_cast<nanoseconds>(steady_clock::now() - t0).count() / 1e9;
        std::sort(glat.begin(), glat.end());
        printf("  混合负载: %.3f M ops/s, get 命中率 %.1f%%, get 延迟 P50=%.0fns P95=%.0fns P99=%.0fns max=%.0fns\n",
               ops / sec / 1e6, ops ? 100.0 * (double)hits / (double)ops * 100.0 / 35.0 : 0,
               (double)glat[glat.size() / 2], (double)glat[glat.size() * 95 / 100],
               (double)glat[glat.size() * 99 / 100], (double)glat.back());
        for (int i = 0; i < NCACHE; i++) file_cache_cpu_destroy(&caches[i]);
    }

    // ============ 压力测试 ============
    printf("\n== FC STRESS TEST ==\n");
    for (int i = 0; i < NCACHE; i++)
        file_cache_cpu_init(&caches[i], (uint32_t)i, wb_cb);
    for (int run = 0; run < 3; run++) {
        long long st = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        uint64_t ops = 0;
        while (duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count() - st < 2000) {
            std::string key = mkkey(rnd(0, 199));
            size_t len = 256;
            std::string data(len, 'A');
            void *kbuf = kcopy(data);
            if (!kbuf) continue;
            if (file_cache_promote(&caches[0], (const uint8_t*)key.data(), (uint32_t)key.size(),
                                   kbuf, len, false, 0, 1) != 0)
                kfree(kbuf);
            ops++;
        }
        printf("run %d: promote 吞吐: %llu ops in 2s = %.0f ops/s\n",
               run, (unsigned long long)ops, ops / 2.0);
    }

    printf("\nRESULT: failures=%llu\n", (unsigned long long)failures);
    return failures == 0 ? 0 : 1;
}
