//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//=============================================================
// tests/stdcpp/framework/test_framework.cpp
//   注册表 / 运行器 / 替换后的全局 operator new / delete
//=============================================================
#include "test_framework.h"
#include <cstdlib>
#include <new>

namespace tst {

namespace {

struct reg_ctx {
    test_case* cases;
    std::size_t count;
    std::size_t cap;
};

// 静态初始化顺序问题：注册发生在 main 之前，用 POD 数组 + 线性增长
// （这里刻意不用 stdcpp 的 vector，避免“用被测对象测被测对象”）
static test_case  g_cases[2048];
static std::size_t g_count = 0;
static std::size_t g_failures = 0;

static const char* g_current = "";

// ---- 分配统计 -------------------------------------------------
static std::size_t g_live_blocks = 0;
static std::size_t g_live_bytes  = 0;
static std::size_t g_total_calls = 0;
static std::size_t g_peak_blocks = 0;
static long        g_fail_after  = -1;   // -1 关闭

} // namespace

void register_test(const char* name, const char* file, test_fn fn) {
    if (g_count >= sizeof(g_cases) / sizeof(g_cases[0])) return;
    g_cases[g_count].name = name;
    g_cases[g_count].file = file;
    g_cases[g_count].fn   = fn;
    ++g_count;
}

std::size_t alloc_live_blocks() { return g_live_blocks; }
std::size_t alloc_live_bytes()  { return g_live_bytes; }
std::size_t alloc_total_calls() { return g_total_calls; }
std::size_t alloc_peak_blocks() { return g_peak_blocks; }
void alloc_reset_peaks() { g_peak_blocks = g_live_blocks; }

void alloc_fail_after(long n) { g_fail_after = n; }
void alloc_fail_off() { g_fail_after = -1; }
bool alloc_failing() { return g_fail_after >= 0; }

void detail_alloc_track(std::size_t bytes, int delta) {
    if (delta > 0) {
        ++g_live_blocks;
        g_live_bytes += bytes;
        if (g_live_blocks > g_peak_blocks) g_peak_blocks = g_live_blocks;
    } else {
        if (g_live_blocks) --g_live_blocks;
        if (g_live_bytes >= bytes) g_live_bytes -= bytes;
        else g_live_bytes = 0;
    }
}

void detail_alloc_total() { ++g_total_calls; }

bool detail_alloc_should_fail() {
    if (g_fail_after < 0) return false;
    if (g_fail_after == 0) return true;
    --g_fail_after;
    return false;
}

void fail(const char* file, int line, const char* expr, const char* detail) {
    ++g_failures;
    std::printf("  [FAIL] %s\n         at %s:%d\n         expr: %s\n",
                g_current, file, line, expr);
    if (detail) std::printf("         %s\n", detail);
    std::fflush(stdout);
}

int run_all(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    std::size_t ran = 0;

    std::printf("stdcpp test suite: %zu case(s) registered\n", g_count);
    for (std::size_t i = 0; i < g_count; ++i) {
        if (filter && !std::strstr(g_cases[i].name, filter)) continue;
        g_current = g_cases[i].name;
        const std::size_t before = g_failures;
        const std::size_t live_before = g_live_blocks;
        g_cases[i].fn();
        ++ran;
        const bool ok = (g_failures == before);
        std::printf("%s %s\n", ok ? "  [ ok ]" : "  [FAIL]", g_cases[i].name);
        if (g_live_blocks != live_before) {
            std::printf("         [warn] case leaked %zu block(s)\n",
                        g_live_blocks - live_before);
            ++g_failures;
        }
    }

    std::printf("\n%zu case(s) ran, %zu failure(s)\n", ran, g_failures);
    if (ran == 0) {
        std::printf("no case matched filter '%s'\n", filter ? filter : "");
        return 2;
    }
    return g_failures ? 1 : 0;
}

} // namespace tst

//=============================================================
// 全局分配函数替换
//
// 布局: [size_t size][payload...]，delete 时回读 size 并归还。
// 用 malloc/free 作为底层，避免与统计逻辑递归。
//=============================================================
namespace {

struct alignas(std::max_align_t) block_header {
    std::size_t size;
    std::size_t magic;
};

const std::size_t kMagic = 0x53544443u;   // 'STDC'

inline void* raw_alloc(std::size_t n) {
    tst::detail_alloc_total();
    if (tst::detail_alloc_should_fail())
        throw std::bad_alloc();

    block_header* h = static_cast<block_header*>(std::malloc(sizeof(block_header) + n));
    if (!h) throw std::bad_alloc();
    h->size = n;
    h->magic = kMagic;
    tst::detail_alloc_track(n, +1);
    return h + 1;
}

inline void raw_free(void* p) {
    if (!p) return;
    block_header* h = static_cast<block_header*>(p) - 1;
    if (h->magic != kMagic) {
        std::printf("  [FATAL] operator delete on non-heap pointer %p\n", p);
        std::abort();
    }
    h->magic = 0;
    tst::detail_alloc_track(h->size, -1);
    std::free(h);
}

} // namespace

void* operator new(std::size_t n) { return raw_alloc(n); }
void* operator new[](std::size_t n) { return raw_alloc(n); }
void  operator delete(void* p) noexcept { raw_free(p); }
void  operator delete[](void* p) noexcept { raw_free(p); }
void  operator delete(void* p, std::size_t) noexcept { raw_free(p); }
void  operator delete[](void* p, std::size_t) noexcept { raw_free(p); }
