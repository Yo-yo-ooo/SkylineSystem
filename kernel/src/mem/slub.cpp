//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <mem/heap.h>
#include <mem/pmm.h>

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>
#endif
#include <pdef.h>

#include "heap_internal.h"

// ==================== SLUB: named per-CPU object caches ====================
#ifdef __x86_64__
// Complements the size-class SLAB backend above: a kmem_cache serves one fixed
// object type. Allocation/free on the current CPU's active slab is lock-free
// (embedded Treiber freelist, CAS-driven); only slab refill/reclaim takes the
// cache-wide lock. Each slab is one page, so object->slab is a page-align away.
#define SLUB_PAGE_MAGIC 0x534C5542UL   /* 'SLUB' */

struct slub_slab_t {
    uint64_t       magic;
    kmem_cache    *cache;
    slub_slab_t   *partial_next;   // global partial-list link
    void          *freelist;       // embedded lock-free free-object stack
    uint32_t       inuse;
    uint32_t       objects;
    int32_t        cpu_owner;      // active owner CPU id, -1 when unowned
    bool           on_partial;
};

struct kmem_cache {
    const char    *name;
    uint32_t       obj_size;
    uint32_t       area_off;
    uint32_t       objs_per_slab;
    spinlock_t     lock;
    slub_slab_t   *partial;
    uint64_t       nr_partial;
    slub_slab_t   *cpu_active[MAX_CPU];
    uint64_t       alloc_count;
    uint64_t       free_count;
    uint64_t       refill_count;
#ifdef __KERNEL_TEST_HOST__
    uint64_t       slow_count;   // 测试期慢路径计数
#endif
};

static inline slub_slab_t *slub_page_of(void *obj) {
    return (slub_slab_t *)((uint64_t)obj & ~(uint64_t)(PAGE_SIZE - 1));
}

// Lock-free LIFO over the link word stored inside each free object.
/* 注: 曾尝试 64 位 tagged 指针(低 48 位地址+高 16 位代数)防 ABA —— 对宿主
   测试有效, 但内核 HHDM 地址(0xFFFF_8xxx...)高 16 位=0xFFFF, 打包会截断
   地址 → 解包出非规范指针, QEMU 实测 #GP(slub_stack_pop, 核数扫描复现)。
   已回滚为裸指针 CAS。跨 CPU 同 slab 的 ABA 理论风险与上游一致
   (Linux 以 per-CPU freelist 规避), 本内核当前经 4 线程 TSAN 混沌未复现,
   记为已知限制, 路线图: per-CPU freelist 重构。 */
static inline void slub_stack_push(void **head, void *obj) {
    void *cur;
    do {
        cur = __atomic_load_n(head, __ATOMIC_ACQUIRE);
        *(void **)obj = cur;
    } while (!__atomic_compare_exchange_n(head, &cur, obj, false,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}
static inline void *slub_stack_pop(void **head) {
    void *cur, *next;
    do {
        cur = __atomic_load_n(head, __ATOMIC_ACQUIRE);
        if (!cur) return nullptr;
        next = *(void **)cur;
    } while (!__atomic_compare_exchange_n(head, &cur, next, false,
                                          __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
    return cur;
}

static slub_slab_t *slub_new_slab(kmem_cache *c) {
    slub_slab_t *s = (slub_slab_t *)VMM::Alloc(kernel_pagemap, 1, false);
    if (!s) return nullptr;

    void *head = nullptr;
    uint32_t n = 0;
    char *base = (char *)s;
    for (uint32_t off = c->area_off; off + c->obj_size <= PAGE_SIZE; off += c->obj_size) {
        void *obj = base + off;
        *(void **)obj = head;
        head = obj;
        ++n;
    }
    s->magic = SLUB_PAGE_MAGIC;
    s->cache = c;
    s->partial_next = nullptr;
    s->freelist = head;
    s->inuse = 0;
    s->objects = n;
    s->cpu_owner = -1;
    s->on_partial = false;
    return s;
}

namespace SLUB {

// Slow path, entered with local IRQs already masked. Refills (or replaces) the
// per-CPU active slab and returns one object.
static void *slow_alloc(kmem_cache *c, cpu_t *cpu, slub_slab_t *old) {
#ifdef __KERNEL_TEST_HOST__
    /* 宿主测试: 统计慢路径次数 —— 与延迟离群点关联归因 */
    __atomic_add_fetch(&c->slow_count, 1, __ATOMIC_RELAXED);
#endif
    spinlock_lock(&c->lock);

    if (old) {
        // A remote free can have repopulated the supposedly exhausted slab.
        void *revived = slub_stack_pop(&old->freelist);
        if (revived) { spinlock_unlock(&c->lock); return revived; }
        if (cpu && cpu->id < MAX_CPU && c->cpu_active[cpu->id] == old)
            c->cpu_active[cpu->id] = nullptr;
        __atomic_store_n(&old->cpu_owner, -1, __ATOMIC_RELEASE);
        // old is full and on no list; later frees relink it as partial.
    }

    slub_slab_t *s = c->partial;
    if (s) {
        c->partial = s->partial_next;
        c->nr_partial--;
        s->on_partial = false;
        __atomic_add_fetch(&c->refill_count, 1, __ATOMIC_RELAXED);
    } else {
        s = slub_new_slab(c);
        if (!s) { spinlock_unlock(&c->lock); return nullptr; }
    }

    if (cpu && cpu->id < MAX_CPU) {
        c->cpu_active[cpu->id] = s;
        __atomic_store_n(&s->cpu_owner, (int32_t)cpu->id, __ATOMIC_RELEASE);
    } else {
        // No per-CPU context (early boot): borrow one, return the rest to partial.
        __atomic_store_n(&s->cpu_owner, -1, __ATOMIC_RELEASE);
    }

    void *obj = slub_stack_pop(&s->freelist);

    if (!cpu || cpu->id >= MAX_CPU) {
        if (s->freelist) {
            s->on_partial = true;
            s->partial_next = c->partial;
            c->partial = s;
            c->nr_partial++;
        }
    }
    spinlock_unlock(&c->lock);
    return obj;
}

kmem_cache *Create(const char *name, size_t obj_size, size_t align) {
    if (obj_size == 0) return nullptr;
    if (align == 0 || (align & (align - 1)) != 0) return nullptr;
    if (align < 8) align = 8;                 // an embedded link needs 8 bytes
    uint32_t os  = (uint32_t)SLAB_ALIGN_UP(obj_size, align);
    uint32_t off = (uint32_t)SLAB_ALIGN_UP(sizeof(slub_slab_t), align);
    if ((uint64_t)off + os > PAGE_SIZE) return nullptr;  // header+object per page

    kmem_cache *c = (kmem_cache *)SLAB::Alloc(sizeof(kmem_cache));
    if (!c) return nullptr;
    _memset(c, 0, sizeof(*c));
    c->name = name;
    c->obj_size = os;
    c->area_off = off;
    c->objs_per_slab = (uint32_t)((PAGE_SIZE - off) / os);
    c->lock = 0;
    c->partial = nullptr;
    c->nr_partial = 0;
    return c;
}

void *Alloc(kmem_cache *c) {
    if (unlikely(!c)) return nullptr;
    uint64_t flags = irq_save();
    cpu_t *cpu = this_cpu();
    slub_slab_t *s = (cpu && cpu->id < MAX_CPU) ? c->cpu_active[cpu->id] : nullptr;

    void *obj = s ? slub_stack_pop(&s->freelist) : nullptr;
    if (unlikely(!obj)) obj = slow_alloc(c, cpu, s);

    if (likely(obj)) {
        slub_slab_t *owner = slub_page_of(obj);
        __atomic_add_fetch(&owner->inuse, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&c->alloc_count, 1, __ATOMIC_RELAXED);
    }
    irq_restore(flags);
    return obj;
}

// A slab just left the fully-used state; make it available to other CPUs.
// The owner/inuse snapshot taken by the caller is only a fast filter: under the
// lock we must re-validate, because a concurrent slow_alloc may have re-homed
// this slab onto a CPU as its active slab (which must never be listed/freed).
static void link_partial_locked(kmem_cache *c, slub_slab_t *s) {
    spinlock_lock(&c->lock);
    if (unlikely(s->magic != SLUB_PAGE_MAGIC)) { spinlock_unlock(&c->lock); return; }  /* 已被并发 drain 回收 */
    int32_t owner = __atomic_load_n(&s->cpu_owner, __ATOMIC_ACQUIRE);
    uint32_t inuse = __atomic_load_n(&s->inuse, __ATOMIC_ACQUIRE);
    if (owner < 0 && !s->on_partial && inuse > 0 && inuse < s->objects) {
        s->partial_next = c->partial;
        c->partial = s;
        s->on_partial = true;
        c->nr_partial++;
    } else if (owner < 0 && !s->on_partial && inuse == 0) {
        // Drained completely before we got the lock: reclaim right away.
        s->magic = 0;
        spinlock_unlock(&c->lock);
        VMM::Free(kernel_pagemap, s);
        return;
    }
    spinlock_unlock(&c->lock);
}

// Detach from partial (if linked) and return the slab page to the VMM.
static void reclaim_slab(kmem_cache *c, slub_slab_t *s) {
    spinlock_lock(&c->lock);
    if (unlikely(s->magic != SLUB_PAGE_MAGIC)) { spinlock_unlock(&c->lock); return; }  /* 已被并发 drain 回收 */
    // Re-validate under the lock: the slab could have been re-homed onto a CPU
    // (active slab) or refilled by a concurrent alloc; never free it then.
    int32_t owner = __atomic_load_n(&s->cpu_owner, __ATOMIC_ACQUIRE);
    uint32_t inuse = __atomic_load_n(&s->inuse, __ATOMIC_ACQUIRE);
    if (owner >= 0 || inuse != 0) { spinlock_unlock(&c->lock); return; }
    if (s->on_partial) {
        slub_slab_t **pp = &c->partial;
        while (*pp && *pp != s) pp = &(*pp)->partial_next;
        if (*pp == s) { *pp = s->partial_next; c->nr_partial--; }
        s->on_partial = false;
    }
    s->magic = 0;
    spinlock_unlock(&c->lock);
    VMM::Free(kernel_pagemap, s);
}

void Free(kmem_cache *c, void *obj) {
    if (unlikely(!c || !obj)) return;
    slub_slab_t *s = slub_page_of(obj);
    if (unlikely(s->magic != SLUB_PAGE_MAGIC || s->cache != c))
        slab_fatal("SLUB error: free of an object not owned by this cache\n");
    uint64_t flags = irq_save();
    uint32_t before = (uint32_t)__atomic_fetch_sub(&s->inuse, 1u, __ATOMIC_ACQ_REL);
    /* 修复: inuse==0 表示双重释放 —— 原实现会让 inuse 下溢并把对象再次
       压入空闲链(自环), 同一指针被无限次重复分配; 回滚计数并拒绝 */
    if (unlikely(before == 0)) {
        __atomic_add_fetch(&s->inuse, 1u, __ATOMIC_ACQ_REL);
        irq_restore(flags);
        slab_fatal("SLUB error: double free detected (inuse==0)\n");
        return;
    }
    uint32_t after  = before - 1;
    slub_stack_push(&s->freelist, obj);
    __atomic_add_fetch(&c->free_count, 1, __ATOMIC_RELAXED);
    int32_t owner = __atomic_load_n(&s->cpu_owner, __ATOMIC_ACQUIRE);

    if (after == 0) {
        // Fully drained. An unowned slab is reclaimed; a CPU's active slab stays
        // cached (even when empty) for immediate locality on the next alloc.
        if (owner < 0) reclaim_slab(c, s);
    } else if (before == s->objects) {
        // full -> partial: I am the first free; only unowned slabs join the list.
        if (owner < 0) link_partial_locked(c, s);
    }
    irq_restore(flags);
}

// Caller guarantees no object of the cache is still in use.
void Destroy(kmem_cache *c) {
    if (unlikely(!c)) return;
    spinlock_lock(&c->lock);
    slub_slab_t *s = c->partial;
    while (s) {
        slub_slab_t *nx = s->partial_next;
        s->magic = 0;
        VMM::Free(kernel_pagemap, s);
        s = nx;
    }
    c->partial = nullptr; c->nr_partial = 0;
    for (uint32_t i = 0; i < MAX_CPU; i++) {
        if (c->cpu_active[i]) {
            c->cpu_active[i]->magic = 0;
            VMM::Free(kernel_pagemap, c->cpu_active[i]);
            c->cpu_active[i] = nullptr;
        }
    }
    spinlock_unlock(&c->lock);
    SLAB::Free(c);
}

size_t   ObjectSize(const kmem_cache *c) { return c ? c->obj_size : 0; }
uint64_t AllocCount(const kmem_cache *c) { return c ? __atomic_load_n(&c->alloc_count, __ATOMIC_RELAXED) : 0; }
uint64_t FreeCount(const kmem_cache *c)  { return c ? __atomic_load_n(&c->free_count,  __ATOMIC_RELAXED) : 0; }
#ifdef __KERNEL_TEST_HOST__
uint64_t SlowCount(const kmem_cache *c) { return c ? __atomic_load_n(&c->slow_count, __ATOMIC_RELAXED) : 0; }
uint64_t RefillCount(const kmem_cache *c) { return c ? __atomic_load_n(&c->refill_count, __ATOMIC_RELAXED) : 0; }
#endif

// ---------- Fuse SLUB into the generic kmalloc family ----------
// One fixed-object SLUB cache per SLAB size class (the same 16..1024 power-of-two
// geometry as SLAB::GetCache). kmalloc() serves small objects from these caches;
// larger requests fall back to SLAB's multi-page large-object path. Before
// InitKmalloc() runs (very early boot) every request also falls back to SLAB.
static kmem_cache *g_kmalloc_caches[MAX_SLAB_ORDER];
static bool        g_kmalloc_slub_on = false;

bool InitKmalloc() {
    if (g_kmalloc_slub_on) return true;
    static const char *const names[MAX_SLAB_ORDER] = {
        "kmalloc-16", "kmalloc-32", "kmalloc-64", "kmalloc-128",
        "kmalloc-256", "kmalloc-512", "kmalloc-1024"
    };
    uint32_t sz = 16;
    for (uint32_t i = 0; i < MAX_SLAB_ORDER; i++) {
        g_kmalloc_caches[i] = Create(names[i], sz, sz);   // align == class size
        if (!g_kmalloc_caches[i]) {                       // OOM: roll back, stay on SLAB
            for (uint32_t j = 0; j < i; j++) { Destroy(g_kmalloc_caches[j]); g_kmalloc_caches[j] = nullptr; }
            return false;
        }
        sz <<= 1;
    }
    g_kmalloc_slub_on = true;
    return true;
}

bool KmallocOnline() { return g_kmalloc_slub_on; }

// Self-contained size -> kmalloc class index. Class i holds 16<<i bytes
// (i=0..6 -> 16..1024), identical geometry to SLAB, but computed locally so the
// fuse layer never reads SLAB's caches[] (which is populated later in boot).
static inline uint32_t slub_kmalloc_class(size_t size) {    if (size <= 16) return 0;
    return (uint32_t)(63 - __builtin_clzll((uint64_t)size - 1)) - 3u;
}

#ifdef __KERNEL_TEST_HOST__
kmem_cache *KmallocCacheFor(size_t size) {
    uint32_t idx = slub_kmalloc_class(size);
    return idx < MAX_SLAB_ORDER ? g_kmalloc_caches[idx] : nullptr;
}
#endif

void *Kmalloc(size_t size) {
    if (!g_kmalloc_slub_on) return nullptr;
    if (size == 0) size = 1;
    if (size > MAX_SLAB_SIZE) return nullptr;   // large-object fallback to SLAB
    uint32_t idx = slub_kmalloc_class(size);
    if (idx >= MAX_SLAB_ORDER) return nullptr;
    kmem_cache *c = g_kmalloc_caches[idx];
    if (!c) return nullptr;
    return Alloc(c);
}

// Returns true (and frees) when @obj belongs to a SLUB slab; false means the
// caller must route it through SLAB::Free (SLAB small object / large object).
bool TryFree(void *obj) {
    if (!obj) return true;
    slub_slab_t *s = slub_page_of(obj);
    if (s->magic != SLUB_PAGE_MAGIC) return false;
    Free(s->cache, obj);
    return true;
}

// Usable capacity of a SLUB object, or 0 if @obj is not SLUB-owned.
size_t TryGetSize(void *obj) {
    if (!obj) return 0;
    slub_slab_t *s = slub_page_of(obj);
    if (s->magic != SLUB_PAGE_MAGIC || !s->cache) return 0;
    return s->cache->obj_size;
}

// Built-in smoke test, run once from arch init right after SLAB::Init (so SLAB
// and the kernel pagemap are live and the kmalloc caches are already online).
// It drives the public C ABI plus the fused kmalloc family end to end.
static uint32_t g_fail_stage = 0;
uint32_t LastFailStage() { return g_fail_stage; }

bool SelfTest() {
    #define STF(n) do { g_fail_stage = (n); kmem_cache_destroy(c); return false; } while (0)
    kmem_cache *c = kmem_cache_create("slub_selftest", 48, 16);
    if (!c) { g_fail_stage = 1; return false; }

    void *obj[16];
    for (int i = 0; i < 16; i++) {
        obj[i] = kmem_cache_alloc(c);
        if (!obj[i]) STF(2);
        if (((uint64_t)obj[i] & 15u) != 0) STF(3);
        *(volatile uint64_t *)obj[i] = 0x5A5A0000ull + (uint64_t)i;
    }
    for (int i = 0; i < 16; i++)
        for (int j = i + 1; j < 16; j++)
            if (obj[i] == obj[j]) STF(4);

    for (int i = 0; i < 16; i++) kmem_cache_free(c, obj[i]);

    void *q = kmem_cache_alloc(c);   // reuse the just-freed slab
    if (!q) STF(5);
    kmem_cache_free(c, q);

    // ---- kmalloc-family fusion (InitKmalloc is online by now) ----
    // Small requests must be served by SLUB pages; kfree routes back by magic.
    static const uint32_t probe[14] = {1,8,15,16,17,32,63,64,128,255,256,512,1000,1024};
    void *fq[16];
    for (uint32_t i = 0; i < 14; i++) {
        fq[i] = kmalloc(probe[i]);
        if (!fq[i]) STF(20);
        if (slub_page_of(fq[i])->magic != SLUB_PAGE_MAGIC) STF(21);
        if (((uint64_t)fq[i] & 15u) != 0) STF(22);
        *(volatile uint64_t *)fq[i] = 0xC0FFEE00ull + i;
    }
    for (uint32_t i = 0; i < 14; i++) kfree(fq[i]);

    // krealloc grows and preserves contents
    char *kold = (char *)kmalloc(32);
    if (!kold) STF(30);
    for (int i = 0; i < 32; i++) kold[i] = (char)(i & 0x7F);
    char *knew = (char *)krealloc(kold, 500);
    if (!knew) STF(31);
    for (int i = 0; i < 32; i++)
        if (knew[i] != (char)(i & 0x7F)) STF(32);
    kfree(knew);

    // aligned allocation is aligned to the requested power-of-two
    void *al = kmalloc_aligned(40, 64);
    if (!al || (((uint64_t)al & 63u) != 0)) STF(40);
    kfree(al);

    // A large (>1024 B) request stays on SLAB multi-page pages; kfree must still
    // route it correctly (its header magic is not SLUB).
    void *big = kmalloc(4096);
    if (!big) STF(50);
    uint64_t *big_hdr = (uint64_t *)((uint64_t)big & ~(uint64_t)(PAGE_SIZE - 1));
    if (*big_hdr != (uint64_t)LARGE_PAGE_MAGIC) STF(51);
    *(volatile uint64_t *)big = 0xB16B16;
    kfree(big);

    kmem_cache_destroy(c);
    #undef STF

    // --gc-sections collects global functions whose only calls were inlined
    // away. Hold the address of every public entry point through a volatile
    // table so they stay callable from other translation units. The read is
    // volatile (cannot be elided); the branch is never taken at runtime.
    static void *const volatile api_anchor[] = {
        (void *)&kmem_cache_create, (void *)&kmem_cache_alloc,
        (void *)&kmem_cache_free,   (void *)&kmem_cache_destroy,
        (void *)&Create, (void *)&Alloc, (void *)&Free, (void *)&Destroy,
        (void *)&ObjectSize, (void *)&AllocCount, (void *)&FreeCount,
    };
    if (api_anchor[0] == (void *)1) { g_fail_stage = 60; return false; }
    return true;
}

} // namespace SLUB

#endif // __x86_64__ (SLUB 段, 原 heap.cpp 786 处 #ifdef 的闭合)
