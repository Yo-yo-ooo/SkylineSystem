//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <mem/heap.h>
#include <mem/pmm.h>

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>
#endif
#include <pdef.h>

#include "heap_internal.h"

#ifdef __x86_64__
extern "C" kmem_cache *kmem_cache_create(const char *n, uint64_t sz, uint64_t al) { return SLUB::Create(n, sz, al ? al : 8); }
extern "C" void        kmem_cache_destroy(kmem_cache *c) { SLUB::Destroy(c); }
extern "C" void       *kmem_cache_alloc(kmem_cache *c) { return SLUB::Alloc(c); }
extern "C" void        kmem_cache_free(kmem_cache *c, void *o) { SLUB::Free(c, o); }
#endif // __x86_64__

#ifdef __KERNEL_TEST_HOST__
/* 宿主测试(ASAN): fc_entry_free 毒化已释放对象; 分配时解毒,
   使 ASAN 只报告"释放后未经复用即被访问"的真实 UAF */
extern "C" void __asan_unpoison_memory_region(const volatile void *addr, size_t size);

/* 全量分配环形日志: 诊断"活地址被再次分配/覆盖"。
   显式开关: 单线程套件开启; 多线程/TSAN 套件关闭(避免日志写入竞态)。 */
struct _kalloc_rec { uintptr_t p; uint32_t size; uint32_t ra; };
static struct _kalloc_rec g_kalloc_log[1 << 20];
static uint32_t g_kalloc_idx = 0;
static int g_kalloc_log_on = 0;
extern "C" void kalloc_log_enable(int on) { g_kalloc_log_on = on; }
extern "C" void kalloc_query(void *p) {
    printf_("[KMQ] 查询 %p 的全部分配记录 (最近 2^20 次):\n", p);
    uint32_t found = 0;
    for (uint32_t i = 0; i < (1u << 20); i++) {
        if (g_kalloc_log[i].p == (uintptr_t)p) {
            printf_("[KMQ]   p=%p size=%u ra=0x%x\n", p, g_kalloc_log[i].size, g_kalloc_log[i].ra);
            if (++found >= 8) break;
        }
    }
    printf_("[KMQ] 共 %u 条\n", found);
}
#endif

extern "C" void *kmalloc(uint64_t size) {
#ifdef __x86_64__
    void *p = SLUB::Kmalloc((size_t)size);    // small object: lock-free SLUB path
    if (likely(p)) {
#ifdef __KERNEL_TEST_HOST__
        __asan_unpoison_memory_region(p, (size_t)size);
        if (g_kalloc_log_on)
            g_kalloc_log[g_kalloc_idx++ & ((1u << 20) - 1)] = {(uintptr_t)p, (uint32_t)size, (uint32_t)(uintptr_t)__builtin_return_address(0)};
#endif
        return p;
    }
#endif
    void *q = SLAB::Alloc(size);               // >1024 B large object, or SLUB not online
#ifdef __KERNEL_TEST_HOST__
    if (q) {
        __asan_unpoison_memory_region(q, (size_t)size);
        if (g_kalloc_log_on)
            g_kalloc_log[g_kalloc_idx++ & ((1u << 20) - 1)] = {(uintptr_t)q, (uint32_t)size, (uint32_t)(uintptr_t)__builtin_return_address(0)};
    }
#endif
    return q;
}

extern "C" void kfree(void *ptr) {
#ifdef __x86_64__
    if (SLUB::TryFree(ptr)) return;           // page magic routes SLUB vs SLAB/large
#endif
    SLAB::Free(ptr);
}

extern "C" void *krealloc(void *ptr, uint64_t size) {
    if (!ptr) return kmalloc(size);
    if (size == 0) { kfree(ptr); return nullptr; }
#ifdef __x86_64__
    size_t cap = SLUB::TryGetSize(ptr);
    if (cap) {                                // SLUB-backed object
        if ((uint64_t)cap >= size) return ptr;
        void *np = kmalloc(size);
        if (np) { __memcpy(np, ptr, cap); kfree(ptr); }
        return np;
    }
#endif
    return SLAB::Realloc(ptr, size);          // SLAB/large object keeps its semantics
}

extern "C" void *kmalloc_aligned(uint64_t size, uint64_t align) {
#ifdef __x86_64__
    if (SLUB::KmallocOnline()) {
        if (align == 0 || (align & (align - 1)) != 0) return nullptr;
        uint64_t need = (size > align) ? size : align;
        void *p = SLUB::Kmalloc((size_t)need);
        if (p) return p;                      // power-of-two class >= need >= align
    }
#endif
    return SLAB::AllocAligned(size, align);
}

uint64_t GetPtrPointAreaSize(void *ptr) {
#ifdef __x86_64__
    size_t s = SLUB::TryGetSize(ptr);
    if (s) return (uint64_t)s;
#endif
    return SLAB::GetSize(ptr, false);
}

extern "C" void *kcalloc(size_t numitems, size_t size) {
    if (numitems == 0 || size == 0) return nullptr;

    size_t total;
    if (__builtin_mul_overflow(numitems, size, &total)) {
        return nullptr;
    }

    void *ptr = kmalloc(total);
    if (ptr) {
        _memset(ptr, 0, total);
    }
    return ptr;
}