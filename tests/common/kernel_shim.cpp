// tests/common/kernel_shim.cpp — 内核原语 → 宿主实现
#include "kernel_shim.h"
#include "arch/x86_64/vmm/vmm.h"
#include "arch/x86_64/smp/smp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <atomic>
#include <mutex>
#include <unordered_map>

// ---------------- 基础内存函数 ----------------
extern "C" void *__memcpy(void *d, const void *s, uint64_t n) { return memcpy(d, s, (size_t)n); }
extern "C" void _memset(void *dest, uint8_t value, uint64_t size) { memset(dest, (int)value, (size_t)size); }
extern "C" void _memmove(void *dest, void *src, uint64_t size) { memmove(dest, src, (size_t)size); }
extern "C" int32_t _memcmp(const void *a, const void *b, size_t size) { return memcmp(a, b, size); }

// ---------------- 自旋锁 (int32_t) ----------------
static inline void spin_pause() {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#endif
}
extern "C" void spinlock_lock(int32_t *l) {
    while (__sync_lock_test_and_set((volatile int32_t*)l, 1)) spin_pause();
}
extern "C" void spinlock_unlock(int32_t *l) {
    __sync_lock_release((volatile int32_t*)l);
}

// ---------------- panic / 位图 ----------------
extern "C" void Panic(const char *message) {
    fprintf(stderr, "[PANIC] %s\n", message);
    abort();
}
void hcf() { Panic("hcf"); }
void bitmap_set(uint8_t *bmp, uint64_t bit) { bmp[bit >> 3] |= (1u << (bit & 7)); }
void bitmap_clear(uint8_t *bmp, uint64_t bit) { bmp[bit >> 3] &= ~(1u << (bit & 7)); }
bool bitmap_get(uint8_t *bmp, uint64_t bit) { return (bmp[bit >> 3] >> (bit & 7)) & 1; }

// ---------------- 日志 (kprintf.h 宏的底层实现) ----------------
extern "C" int32_t printf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    return 0;
}
extern "C" int32_t snprintf_(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int rc = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return rc;
}

// ---------------- VMM 页池（记账泄漏） ----------------
pagemap_t *kernel_pagemap = nullptr;
static std::mutex g_page_lock;
static std::unordered_map<void*, uint64_t> g_live_pages;
static std::unordered_map<void*, uintptr_t> g_live_ra;
static uint64_t g_live_page_count = 0;
static uintptr_t g_heap_lo = ~(uintptr_t)0, g_heap_hi = 0;
static uint32_t g_fail_next = 0;
static int g_fail_all = 0;

/* LSAN 支持: 进程退出时归还全部 shim 页池 —— 之后 LSAN 报告的泄漏即真实泄漏。
   注: 不要调用 g_live_pages.clear() —— ASAN 下 free 池页后对桶数组的 memset
   会误报 UAF; 进程即将退出, 无需清表。 */
__attribute__((destructor)) static void shim_free_all_pages(void) {
    std::lock_guard<std::mutex> g(g_page_lock);
    for (auto &kv : g_live_pages) free(kv.first);
    g_live_page_count = 0;
}

extern "C" void vmm_fail_next_alloc(uint32_t n) { g_fail_next = n; }
extern "C" void vmm_set_fail_all(int on) { g_fail_all = on; }
extern "C" void dump_live_pages(void) {
    std::lock_guard<std::mutex> g(g_page_lock);
    printf("== LIVE PAGES (%llu total) ==\n", (unsigned long long)g_live_page_count);
    for (auto &kv : g_live_pages) {
        printf("  page=%p npages=%llu alloc_site_ra=0x%llx\n",
               kv.first, (unsigned long long)kv.second,
               (unsigned long long)g_live_ra[kv.first]);
    }
}

namespace VMM {
    void *Alloc(pagemap_t *pm, uint64_t npages, bool user) {
        (void)pm; (void)user;
        if (g_fail_all || (g_fail_next > 0)) {
            if (g_fail_next > 0) g_fail_next--;
            return nullptr;
        }
        size_t bytes = (size_t)npages * 4096;
        void *p = nullptr;
        if (posix_memalign(&p, 4096, bytes) != 0) return nullptr;
        memset(p, 0, bytes);
        std::lock_guard<std::mutex> g(g_page_lock);
        g_live_pages[p] = npages;
        g_live_ra[p] = (uintptr_t)__builtin_return_address(0);
        g_live_page_count += npages;
        uintptr_t a = (uintptr_t)p, b = a + bytes;
        if (a < g_heap_lo) g_heap_lo = a;
        if (b > g_heap_hi) g_heap_hi = b;
        return p;
    }
    void Free(pagemap_t *pm, void *ptr) {
        (void)pm;
        if (!ptr) return;
        std::lock_guard<std::mutex> g(g_page_lock);
        auto it = g_live_pages.find(ptr);
        if (it != g_live_pages.end()) {
            g_live_page_count -= it->second;
            g_live_pages.erase(it);
        }
        free(ptr);
    }
    uint64_t LivePages() {
        std::lock_guard<std::mutex> g(g_page_lock);
        return g_live_page_count;
    }
    bool HeapContains(const void *p) {
        uintptr_t a = (uintptr_t)p;
        return a >= g_heap_lo && a < g_heap_hi;
    }
}

// 线程局部 per-CPU: 多线程测试中每个宿主线程 = 一个虚拟 CPU (id 由 test_cpu_set_id 设置)
static thread_local cpu_t g_tls_cpu;
void test_cpu_set_id(uint32_t id) { g_tls_cpu.id = id; }
cpu_t *this_cpu() { return &g_tls_cpu; }
cpu_t *get_cpu(uint32_t i) { (void)i; return &g_tls_cpu; }
uint64_t g_test_op = 0;

// 非 ASAN 构建下 fc.cpp 测试代码引用的 ASAN 接口空实现 (ASAN 构建由运行时提供)
#if !defined(__SANITIZE_ADDRESS__)
extern "C" void __asan_poison_memory_region(const volatile void *, size_t) {}
extern "C" void __asan_unpoison_memory_region(const volatile void *, size_t) {}
#endif

// klib.h 里声明但宿主无实现的其余符号
volatile uint64_t hhdm_offset = 0;
volatile uint64_t RSDP_ADDR = 0;
uint32_t MaxXsaveSize = 0;
