//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <mem/heap.h>
#include <mem/pmm.h>

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>
#endif
#include <pdef.h>


#define SLAB_ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((uint64_t)(a) - 1))

#define SLAB_PAGES 1
#define SLAB_SIZE (SLAB_PAGES * PAGE_SIZE)

#ifndef MAX_SLAB_ORDER
#define MAX_SLAB_ORDER 7
#endif
static_assert(MAX_SLAB_ORDER >= 1 && MAX_SLAB_ORDER <= 16, "bad MAX_SLAB_ORDER");
#define MAX_SLAB_SIZE (16u << (MAX_SLAB_ORDER - 1))

// Per-CPU 水位线调优：当前回流阈值为 2 * SLAB_BATCH。
#define SLAB_BATCH 16
#define DRAIN_HIGH_WATERMARK (SLAB_BATCH * 4)
#define DRAIN_LOW_WATERMARK (SLAB_BATCH * 2)
#define EMPTY_CACHE_LIMIT 4

// 页池水位：池内 ≥16 页时归还到 4 页
#define PAGE_POOL_HIGH_WATERMARK 16
#define PAGE_POOL_LOW_WATERMARK  4

#define SLAB_PAGE_MAGIC  0x50414745
#define LARGE_PAGE_MAGIC 0x51424D55

// 调试开关：页毒化与红区越界检测
#ifdef SLAB_DEBUG_POISON
#define SLAB_POISON_ALLOC 0xAA
#define SLAB_POISON_FREE  0xDD
#define SLAB_REDZONE_MAGIC 0xDEADBEEFCAFEBABEULL
#define REDZONE_SIZE 8
#define REDZONE_MIN_OBJ_SIZE 64 // 仅对象 >= 64 字节时启用红区，避免小对象开销过大
#else
#define REDZONE_SIZE 0
#endif

slab_cache_t caches[MAX_SLAB_ORDER];

// ── 新增统计（slab_cache_t 在 pdef.h，不能加字段，用平行数组）──
static uint64_t g_cache_lock_acquires[MAX_SLAB_ORDER];

// ── freepointer 混淆 cookie（启动熵）──
static uint64_t g_freeptr_cookie = 0;

// ── 页池 ──
static slab_page_t *g_pool_head = nullptr;  // FIFO 头 = 最老（复用/释放都从头取）
static slab_page_t *g_pool_tail = nullptr;
static uint32_t     g_pool_count = 0;
static spinlock_t   g_pool_lock = 0;

// ═══════════ 低层工具 ═══════════

static inline uint64_t irq_save() {
    uint64_t flags;
    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    asm volatile("push %0\n\tpopfq" :: "r"(flags) : "memory");
}

static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    uint64_t flags = irq_save();
    spinlock_lock(lock);
    return flags;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    spinlock_unlock(lock);
    irq_restore(flags);
}


static void slab_fatal(const char *msg) __attribute__((noreturn));
static void slab_fatal(const char *msg) {
    Panic(msg);
    for (;;) asm volatile("cli; hlt");
}

// ═══════════ freepointer 混淆 ═══════════
// 页内 freelist 的 next 以 混淆存储在对象前 8 字节：
// 堆溢出砸坏空闲对象的链指针后，弹出时对齐/边界校验必然失败。
// （magazine 与 global_free_list 仍是原始指针链，各自一致即可）

static inline void *fp_encode(void *slot, void *next) {
    return (void *)((uint64_t)next ^ ((uint64_t)slot >> 12) ^ g_freeptr_cookie);
}
static inline void *fp_decode(void *slot, void *encoded) {
    return (void *)((uint64_t)encoded ^ ((uint64_t)slot >> 12) ^ g_freeptr_cookie);
}

// 解码出的 next 必须为 NULL 或落在本页对象栅格上
static inline bool slab_fp_ok(slab_cache_t *cache, slab_page_t *page, void *next) {
    if (!next) return true;
    uint64_t hdr = SLAB_ALIGN_UP(sizeof(slab_page_t), cache->obj_size);
    uint64_t off = (uint64_t)next - (uint64_t)page;
    return (off >= hdr) &&
           (off + cache->obj_size <= SLAB_SIZE) &&
           (((off - hdr) % cache->obj_size) == 0);
}

// ═══════════ 双链表操作 ═══════════

static inline void slab_list_remove(slab_page_t *page, slab_page_t **list_head) {
    if (page->prev) page->prev->next = page->next;
    else *list_head = page->next;
    if (page->next) page->next->prev = page->prev;
    page->next = page->prev = nullptr;
}

static inline void slab_list_insert_head(slab_page_t *page, slab_page_t **list_head) {
    page->prev = nullptr;
    page->next = *list_head;
    if (*list_head) (*list_head)->prev = page;
    *list_head = page;
}

// ═══════════ 调试辅助 ═══════════

#ifdef SLAB_DEBUG_POISON
// 分配出口：红区校验 + alloc 毒化（三条路径统一调用，消除重复）
static void slab_debug_prepare_alloc(slab_cache_t *cache, void *obj) {
    uint64_t usable = cache->obj_size - (cache->has_redzone ? REDZONE_SIZE : 0);
    if (cache->has_redzone) {
        if (*(uint64_t *)((uint64_t)obj + cache->obj_size - REDZONE_SIZE) != SLAB_REDZONE_MAGIC)
            slab_fatal("SLAB error: redzone corrupted before alloc\n");
    }
    if (usable > 8)
        _memset((void *)((uint64_t)obj + 8), SLAB_POISON_ALLOC, usable - 8);
}


static void slab_debug_prepare_free(slab_cache_t *cache, void *ptr) {
    uint64_t usable = cache->obj_size - (cache->has_redzone ? REDZONE_SIZE : 0);
    if (cache->has_redzone) {
        if (*(uint64_t *)((uint64_t)ptr + cache->obj_size - REDZONE_SIZE) != SLAB_REDZONE_MAGIC)
            slab_fatal("SLAB error: buffer overflow detected on free!\n");
    }
    if (usable > 8)
        _memset((void *)((uint64_t)ptr + 8), SLAB_POISON_FREE, usable - 8);
}

//  空页毒化终检（隔离区核心）：空页的所有对象都经历过 free，
//   [8, usable) 必须仍为 0xDD、红区必须仍为魔数。
//   在池入口（旧 cache 几何）与池出口复用前各查一次，
//   滞留期间与回收前的 UAF 写都会在此现形。
static void slab_debug_check_page_poison(slab_cache_t *cache, slab_page_t *page) {
    if (page->magic != SLAB_PAGE_MAGIC || !cache) return;
    uint64_t hdr    = SLAB_ALIGN_UP(sizeof(slab_page_t), cache->obj_size);
    uint64_t usable = cache->obj_size - (cache->has_redzone ? REDZONE_SIZE : 0);
    for (char *obj = (char *)page + hdr;
         obj + cache->obj_size <= (char *)page + SLAB_SIZE;
         obj += cache->obj_size) {
        for (uint64_t i = 8; i < usable; i++) {
            if ((uint8_t)obj[i] != SLAB_POISON_FREE)
                slab_fatal("SLAB error: poison broken (use-after-free on pooled page)\n");
        }
        if (cache->has_redzone) {
            if (*(uint64_t *)(obj + cache->obj_size - REDZONE_SIZE) != SLAB_REDZONE_MAGIC)
                slab_fatal("SLAB error: redzone broken on pooled page\n");
        }
    }
}
#endif

static slab_page_t *PoolPopOldest(void) {
    uint64_t flags = spin_lock_irqsave(&g_pool_lock);
    slab_page_t *p = g_pool_head;
    if (p) {
        g_pool_head = p->next;
        if (g_pool_head) g_pool_head->prev = nullptr;
        else             g_pool_tail = nullptr;
        g_pool_count--;
        p->next = p->prev = nullptr;
    }
    spin_unlock_irqrestore(&g_pool_lock, flags);
    return p;
}

static void PoolReleaseExcess(void) {
    slab_page_t *to_free = nullptr;
    uint64_t flags = spin_lock_irqsave(&g_pool_lock);
    while (g_pool_count > PAGE_POOL_LOW_WATERMARK && g_pool_head) {
        slab_page_t *p = g_pool_head;
        g_pool_head = p->next;
        if (g_pool_head) g_pool_head->prev = nullptr;
        else             g_pool_tail = nullptr;
        g_pool_count--;
        p->prev = nullptr;
        p->next = to_free;
        to_free = p;
    }
    spin_unlock_irqrestore(&g_pool_lock, flags);
    // 锁外归还 VMM（每次自带 shootdown，水位机制保证调用频率被摊薄）
    while (to_free) {
        slab_page_t *p = to_free;
        to_free = p->next;
        VMM::Free(kernel_pagemap, p);
    }
}

// pages：DrainGlobalFreeListLocked 返回的 next 单链，即将脱离 cache
static void slab_release_pages(slab_cache_t *cache, slab_page_t *pages) {
    if (!pages) return;

#ifdef SLAB_DEBUG_POISON
    // 池入口毒化终检（此刻 page->cache 仍是旧 cache，几何匹配）
    for (slab_page_t *p = pages; p; p = p->next)
        slab_debug_check_page_poison(cache, p);
#endif

    bool over = false;
    uint64_t flags = spin_lock_irqsave(&g_pool_lock);
    while (pages) {
        slab_page_t *p = pages;
        pages = p->next;
        __atomic_sub_fetch(&cache->total_pages, 1, __ATOMIC_RELAXED);
        p->prev = nullptr;
        p->next = nullptr;
        if (g_pool_tail) {
            g_pool_tail->next = p;
            p->prev = g_pool_tail;
            g_pool_tail = p;
        } else {
            g_pool_head = g_pool_tail = p;
        }
        g_pool_count++;
    }
    over = (g_pool_count >= PAGE_POOL_HIGH_WATERMARK);
    spin_unlock_irqrestore(&g_pool_lock, flags);

    if (over) PoolReleaseExcess();
}

// ═══════════ 页分配 ═══════════
// 契约：调用方持有 cache->lock（与原实现一致），flags 用于 fatal 路径解锁

static slab_page_t *slab_alloc_page(slab_cache_t *cache, uint64_t flags) {
#ifdef __x86_64__
    //  页池优先：滞留页直接复用，免 VMM 往返 + shootdown
    slab_page_t *page = PoolPopOldest();
    if (page) {
#ifdef SLAB_DEBUG_POISON
        // 隔离期终检：池内滞留期间被 UAF 写过，在此现形
        // （page->cache 尚是旧 cache，与其毒化几何匹配）
        if (page->magic == SLAB_PAGE_MAGIC && page->cache)
            slab_debug_check_page_poison(page->cache, page);
#endif
    } else {
        page = (slab_page_t *)VMM::Alloc(kernel_pagemap, SLAB_PAGES, false);
    }
    if (!page) return nullptr;

    __atomic_add_fetch(&cache->total_pages, 1, __ATOMIC_RELAXED);

    page->magic = SLAB_PAGE_MAGIC;
    page->cache = cache;
    page->next = page->prev = nullptr;
    page->inuse = 0;

    uint64_t header_size = SLAB_ALIGN_UP(sizeof(slab_page_t), cache->obj_size);
    char *ptr = (char *)page + header_size;

    uint64_t avail = SLAB_SIZE - header_size;
    page->objects = avail / cache->obj_size;
    if (page->objects == 0) {
        __atomic_sub_fetch(&cache->total_pages, 1, __ATOMIC_RELAXED);
        VMM::Free(kernel_pagemap, page);
        return nullptr;
    }

    page->freelist = ptr;
    for (uint32_t i = 0; i < page->objects; i++) {
        void *next = (i == page->objects - 1) ? nullptr
                                              : (void *)(ptr + cache->obj_size);
        *(void **)ptr = fp_encode(ptr, next);       //  混淆存储

#ifdef SLAB_DEBUG_POISON
        // 仅大对象初始化红区魔数
        if (cache->has_redzone)
            *(uint64_t *)(ptr + cache->obj_size - REDZONE_SIZE) = SLAB_REDZONE_MAGIC;
#endif
        ptr += cache->obj_size;
    }

    return page;
#else
    (void)cache; (void)flags;
    return nullptr;
#endif
}

// 契约：调用此函数前必须持有 cache->lock！返回待脱离 cache 的页链
static slab_page_t *DrainGlobalFreeListLocked(slab_cache_t *cache, uint64_t flags) {
    if (cache->global_free_count < DRAIN_HIGH_WATERMARK) {
        return nullptr;
    }

    uint64_t to_drain = cache->global_free_count - DRAIN_LOW_WATERMARK;
    slab_page_t *pages_to_free = nullptr;

    for (uint64_t i = 0; i < to_drain; i++) {
        void *obj = cache->global_free_list;
        if (!obj) break;

        cache->global_free_list = *(void **)obj;    // 全局链：原始指针
        cache->global_free_count--;

        slab_page_t *page = (slab_page_t *)((uint64_t)obj & ~(PAGE_SIZE - 1));

        //  全局链完好性：对象必须属于本 cache 的合法 slab 页
        if (page->magic != SLAB_PAGE_MAGIC || page->cache != cache) {
            spin_unlock_irqrestore(&cache->lock, flags);
            slab_fatal("SLAB error: global freelist corrupted\n");
        }

        // 快速路径 Double-Free 检测：inuse 下溢
        if (page->inuse == 0) {
            spin_unlock_irqrestore(&cache->lock, flags);
            slab_fatal("SLAB error: inuse underflow on drain (double free detected)\n");
        }

        // 归还页内 freelist：混淆存储
        *(void **)obj = fp_encode(obj, page->freelist);
        page->freelist = obj;
        page->inuse--;

        if (page->inuse == page->objects - 1) {
            slab_list_remove(page, &cache->full);
            slab_list_insert_head(page, &cache->partial);
        }

        if (page->inuse == 0) {
            slab_list_remove(page, &cache->partial);

            cache->empty_count++;
            page->next = nullptr;
            if (!cache->empty) {
                cache->empty = page;
                cache->empty_tail = page;
                page->prev = nullptr;
            } else {
                page->prev = cache->empty_tail;
                cache->empty_tail->next = page;
                cache->empty_tail = page;
            }

            if (cache->empty_count > EMPTY_CACHE_LIMIT) {
                slab_page_t *p_to_free = cache->empty;
                cache->empty = p_to_free->next;
                if (cache->empty) cache->empty->prev = nullptr;
                else              cache->empty_tail = nullptr;
                cache->empty_count--;

                p_to_free->next = pages_to_free;
                p_to_free->prev = nullptr;
            }
        }
    }
    return pages_to_free;
}

//  统一的「batch → 全局链 → 驱动 → 页池」路径
// （原实现此序列在 4 处重复，现在一处）
static void slab_flush_batch_and_release(slab_cache_t *cache, void **batch, int count) {
    if (!cache || !batch || count <= 0) return;

    uint64_t flags = spin_lock_irqsave(&cache->lock);
    __atomic_add_fetch(&g_cache_lock_acquires[cache->size_class], 1, __ATOMIC_RELAXED);

    for (int j = 0; j < count; j++) {
        *(void **)batch[j] = cache->global_free_list;
        cache->global_free_list = batch[j];
        cache->global_free_count++;
    }

    slab_page_t *pages = DrainGlobalFreeListLocked(cache, flags);
    spin_unlock_irqrestore(&cache->lock, flags);

    slab_release_pages(cache, pages);   //  进页池，而非直接 VMM::Free
}

namespace SLAB {

    void Init() {
#ifdef __x86_64__
        // freepointer cookie：启动熵
        {
            uint32_t lo, hi;
            asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
            g_freeptr_cookie = ((uint64_t)hi << 32) | lo;
            if (g_freeptr_cookie == 0) g_freeptr_cookie = 0x9E3779B97F4A7C15ULL;
        }

        // 页池复位（BSS 已零，显式以防热重入）
        g_pool_head = g_pool_tail = nullptr;
        g_pool_count = 0;
        g_pool_lock = 0;

        uint32_t base_size = 16;
        for (uint32_t i = 0; i < MAX_SLAB_ORDER; i++) {
            caches[i].size_class = i;

#ifdef SLAB_DEBUG_POISON
            caches[i].has_redzone = (base_size >= REDZONE_MIN_OBJ_SIZE);
            caches[i].obj_size = base_size + (caches[i].has_redzone ? REDZONE_SIZE : 0);
#else
            caches[i].has_redzone = false;
            caches[i].obj_size = base_size;
#endif
            caches[i].global_free_list = nullptr;
            caches[i].global_free_count = 0;
            caches[i].partial = nullptr;
            caches[i].full = nullptr;
            caches[i].empty = nullptr;
            caches[i].empty_tail = nullptr;
            caches[i].empty_count = 0;
            caches[i].lock = 0;
            caches[i].alloc_count = 0;
            caches[i].free_count = 0;
            caches[i].cache_hit_count = 0;
            caches[i].total_pages = 0;
            g_cache_lock_acquires[i] = 0;
            base_size <<= 1;
        }
#endif
    }

    /**
     * CPU 热插拔下线钩子
     * 约束：必须在目标 CPU 完全停止调度、且不再执行任何内存分配/释放操作后调用！
     */
    void DrainCpuCache(cpu_t *cpu) {
        if (!cpu) return;
        void *batch[SLAB_BATCH];

        for (uint32_t i = 0; i < MAX_SLAB_ORDER; i++) {
            slab_cache_t *cache = &caches[i];
            int drain_count = 0;

            uint64_t flags = irq_save();
            while (cpu->cslab.count[i] > 0) {
                batch[drain_count++] = cpu->cslab.freelist[i];
                cpu->cslab.freelist[i] = *(void **)batch[drain_count - 1];
                cpu->cslab.count[i]--;

                if (drain_count == SLAB_BATCH) {
                    irq_restore(flags);
                    slab_flush_batch_and_release(cache, batch, drain_count);
                    drain_count = 0;
                    flags = irq_save();
                }
            }

            if (drain_count > 0) {
                irq_restore(flags);
                slab_flush_batch_and_release(cache, batch, drain_count);
            } else {
                irq_restore(flags);
            }
        }
    }

    //  修正上轮结论：size <= 16 特判不是死代码——
    //   size==1 时 size-1==0，clzll(0) 为 UB；
    //   size∈[2,8] 时 bits<3，idx 无符号下溢。必须保留。
    slab_cache_t *GetCache(size_t size) {
        if (size > MAX_SLAB_SIZE) return nullptr;
        if (size <= 16) return &caches[0];

        uint32_t bits = 63 - __builtin_clzll((uint64_t)size - 1);
        uint32_t idx = bits - 3;

        if (idx >= MAX_SLAB_ORDER) return nullptr;
        return &caches[idx];
    }

    // 运行时红区主动校验接口
    void CheckRedzone(void *ptr) {
#ifdef SLAB_DEBUG_POISON
        if (!ptr) return;
        slab_page_t *page = (slab_page_t *)((uint64_t)ptr & ~(PAGE_SIZE - 1));

        if (page->magic == SLAB_PAGE_MAGIC) {
            slab_cache_t *cache = page->cache;
            if (cache && cache->has_redzone) {
                if (*(uint64_t *)((uint64_t)ptr + cache->obj_size - REDZONE_SIZE) != SLAB_REDZONE_MAGIC)
                    slab_fatal("SLAB error: Redzone corrupted!\n");
            }
        } else if (page->magic == LARGE_PAGE_MAGIC) {
            uint64_t usable_size = page->page_count * PAGE_SIZE - sizeof(slab_page_t) - REDZONE_SIZE;
            if (*(uint64_t *)((uint64_t)ptr + usable_size) != SLAB_REDZONE_MAGIC)
                slab_fatal("Critical SLAB error: Large page redzone corrupted!\n");
        }
#endif
    }

    void *Alloc(size_t size) {
#ifdef __x86_64__
        if (size == 0) size = 1;

        // ── 大对象：多页直配，绕过类与页池 ──
        if (size > MAX_SLAB_SIZE) {
            uint64_t total_size;
            if (__builtin_add_overflow(size, sizeof(slab_page_t) + REDZONE_SIZE, &total_size)) {
                return nullptr;
            }

            uint64_t pages = DIV_ROUND_UP(total_size, PAGE_SIZE);
            slab_page_t *page = (slab_page_t *)VMM::Alloc(kernel_pagemap, pages, false);
            if (!page) return nullptr;

            page->magic = LARGE_PAGE_MAGIC;
            page->page_count = pages;

#ifdef SLAB_DEBUG_POISON
            // 大页分配统一启用红区
            uint64_t usable_size = pages * PAGE_SIZE - sizeof(slab_page_t) - REDZONE_SIZE;
            if (usable_size > 0) {
                _memset((void *)(page + 1), SLAB_POISON_ALLOC, usable_size);
                *(uint64_t *)((uint64_t)(page + 1) + usable_size) = SLAB_REDZONE_MAGIC;
            }
#endif
            return (void *)(page + 1);
        }

        slab_cache_t *cache = GetCache(size);
        if (!cache) return nullptr;
        uint32_t idx = cache->size_class;

        // ── 快路径：per-CPU magazine（IF 屏蔽保护；NMI 契约见文件头）──
        uint64_t flags = irq_save();
        cpu_t *cpu = this_cpu();

        if (cpu && cpu->cslab.count[idx] > 0) {
            void *obj = cpu->cslab.freelist[idx];
            cpu->cslab.freelist[idx] = *(void **)obj;    // magazine 链：原始指针
            cpu->cslab.count[idx]--;
            irq_restore(flags);

            __atomic_add_fetch(&cache->alloc_count, 1, __ATOMIC_RELAXED);      //  只记成功
            __atomic_add_fetch(&cache->cache_hit_count, 1, __ATOMIC_RELAXED);
#ifdef SLAB_DEBUG_POISON
            slab_debug_prepare_alloc(cache, obj);
#endif
            return obj;
        }
        irq_restore(flags);

        // ── 慢路径：全局锁，批量补充 magazine ──
        void *batch[SLAB_BATCH];
        int batch_cnt = 0;
        int target_cnt = cpu ? SLAB_BATCH : 1;

        flags = spin_lock_irqsave(&cache->lock);
        __atomic_add_fetch(&g_cache_lock_acquires[idx], 1, __ATOMIC_RELAXED); //  新指标

        for (int i = 0; i < target_cnt; i++) {
            if (cache->global_free_list) {
                batch[i] = cache->global_free_list;
                cache->global_free_list = *(void **)batch[i];   // 全局链：原始指针
                cache->global_free_count--;
            } else {
                if (!cache->partial) {
                    if (cache->empty) {
                        cache->partial = cache->empty;
                        cache->empty = cache->partial->next;
                        if (cache->empty) cache->empty->prev = nullptr;
                        else              cache->empty_tail = nullptr;
                        cache->empty_count--;
                        cache->partial->next = nullptr;
                        cache->partial->prev = nullptr;
                    } else {
                        cache->partial = slab_alloc_page(cache, flags);   //  页池优先
                        if (!cache->partial) break;
                    }
                }
                slab_page_t *page = cache->partial;
                batch[i] = page->freelist;                     // 逻辑头（真实指针）

                //  混淆解码 + 完好性校验：越界砸坏的链指针在此现形
                void *next = fp_decode(batch[i], *(void **)batch[i]);
                if (!slab_fp_ok(cache, page, next)) {
                    spin_unlock_irqrestore(&cache->lock, flags);
                    slab_fatal("SLAB error: freelist pointer corrupted (overflow/double-free)\n");
                }
                page->freelist = next;

                if (page->inuse >= page->objects) {
                    spin_unlock_irqrestore(&cache->lock, flags);
                    slab_fatal("SLAB error: inuse overflow on alloc\n");
                }
                page->inuse++;

                if (!page->freelist) {
                    slab_list_remove(page, &cache->partial);
                    slab_list_insert_head(page, &cache->full);
                }
            }
            batch_cnt++;
        }

        if (batch_cnt == 0) {
            spin_unlock_irqrestore(&cache->lock, flags);
            return nullptr;
        }

        if (cpu) {
            for (int i = 1; i < batch_cnt; i++) {
                *(void **)batch[i] = cpu->cslab.freelist[idx];
                cpu->cslab.freelist[idx] = batch[i];
                cpu->cslab.count[idx]++;
            }
            spin_unlock_irqrestore(&cache->lock, flags);

            __atomic_add_fetch(&cache->alloc_count, 1, __ATOMIC_RELAXED);
#ifdef SLAB_DEBUG_POISON
            slab_debug_prepare_alloc(cache, batch[0]);
#endif
            return batch[0];
        }

        // 无 per-CPU 上下文（早期启动）：只取一个，其余退回全局链
        for (int i = 1; i < batch_cnt; i++) {
            *(void **)batch[i] = cache->global_free_list;
            cache->global_free_list = batch[i];
            cache->global_free_count++;
        }
        spin_unlock_irqrestore(&cache->lock, flags);

        __atomic_add_fetch(&cache->alloc_count, 1, __ATOMIC_RELAXED);
#ifdef SLAB_DEBUG_POISON
        slab_debug_prepare_alloc(cache, batch[0]);
#endif
        return batch[0];
#endif
    }

    void *AllocAligned(size_t size, size_t align) {
        if (align == 0 || (align & (align - 1)) != 0) return nullptr;

#ifdef SLAB_DEBUG_POISON
        if (align > 8) return nullptr;
        return Alloc(size);
#else
        if (align <= 16) return Alloc(size);
        size_t need = (size > align) ? size : align;
        if (need > MAX_SLAB_SIZE) return nullptr;
        return Alloc(need);   // 选中的类 >= need >= align 且为 2 的幂
#endif
    }

    void *Realloc(void *ptr, size_t size) {
#ifdef __x86_64__
        if (!ptr) return SLAB::Alloc(size);
        if (size == 0) { SLAB::Free(ptr); return nullptr; }

        uint64_t current_capacity = SLAB::GetSize(ptr, true);
        if (size <= current_capacity) {
            return ptr;
        }

        void *new_ptr = SLAB::Alloc(size);
        if (new_ptr) {
            uint64_t copy_size = (current_capacity > size) ? size : current_capacity;
            __memcpy(new_ptr, ptr, copy_size);
            SLAB::Free(ptr);
        }
        return new_ptr;
#endif
    }

    void Free(void *ptr) {
#ifdef __x86_64__
        if (!ptr) return;

        slab_page_t *page = (slab_page_t *)((uint64_t)ptr & ~(PAGE_SIZE - 1));

        if (page->magic == LARGE_PAGE_MAGIC) {
            if ((void *)(page + 1) != ptr) {
                slab_fatal("Critical SLAB error: Freeing non-original large page ptr.\n");
            }

#ifdef SLAB_DEBUG_POISON
            uint64_t usable_size = page->page_count * PAGE_SIZE - sizeof(slab_page_t) - REDZONE_SIZE;
            if (usable_size > 0) {
                if (*(uint64_t *)((uint64_t)ptr + usable_size) != SLAB_REDZONE_MAGIC) {
                    slab_fatal("Critical SLAB error: Large page buffer overflow detected on free!\n");
                }
                _memset(ptr, SLAB_POISON_FREE, usable_size);
            }
#endif
            page->magic = 0;
            VMM::Free(kernel_pagemap, page);
            return;
        }

        if (page->magic != SLAB_PAGE_MAGIC) {
            slab_fatal("Critical SLAB error: Trying to free invalid or corrupted ptr.\n");
        }

        slab_cache_t *cache = page->cache;
        __atomic_add_fetch(&cache->free_count, 1, __ATOMIC_RELAXED);
        uint32_t idx = cache->size_class;

        uint64_t header_size = SLAB_ALIGN_UP(sizeof(slab_page_t), cache->obj_size);
        uintptr_t offset = (uintptr_t)ptr - (uintptr_t)page;
        if (offset < header_size || (offset - header_size) % cache->obj_size != 0) {
            slab_fatal("SLAB error: Freeing non-object-aligned pointer.\n");
        }

        uint64_t flags = irq_save();
        cpu_t *cpu = this_cpu();

#ifdef SLAB_DEBUG_POISON
        slab_debug_prepare_free(cache, ptr);
#endif

        if (cpu) {
            *(void **)ptr = cpu->cslab.freelist[idx];
            cpu->cslab.freelist[idx] = ptr;
            cpu->cslab.count[idx]++;

            if (cpu->cslab.count[idx] > SLAB_BATCH * 2) {
                void *batch_to_drain[SLAB_BATCH];
                int drain_count = 0;

                for (int i = 0; i < SLAB_BATCH; i++) {
                    batch_to_drain[i] = cpu->cslab.freelist[idx];
                    if (!batch_to_drain[i]) break;
                    cpu->cslab.freelist[idx] = *(void **)batch_to_drain[i];
                    cpu->cslab.count[idx]--;
                    drain_count++;
                }
                irq_restore(flags);

                slab_flush_batch_and_release(cache, batch_to_drain, drain_count);
                return;
            }
            irq_restore(flags);
        } else {
            irq_restore(flags);
            slab_flush_batch_and_release(cache, &ptr, 1);
        }
#endif
    }

    void *UserAlloc(size_t size) {
        return SLAB::Alloc(size);
    }

    uint64_t GetSize(void *ptr, bool ERO) {
#ifdef __x86_64__
        if (!ptr || (uint64_t)ptr < 0x1000) return 0;

        slab_page_t *page = (slab_page_t *)((uint64_t)ptr & ~(PAGE_SIZE - 1));

        if (page->magic == LARGE_PAGE_MAGIC) {
            return page->page_count * PAGE_SIZE - sizeof(slab_page_t) - REDZONE_SIZE;
        }
        if (page->magic == SLAB_PAGE_MAGIC) {
            if (page->cache) {
                return page->cache->obj_size - (page->cache->has_redzone ? REDZONE_SIZE : 0);
            }
        }

        if (!ERO) slab_fatal("Invalid SLAB ptr in GetSize\n");
        return 0;
#else
        return 0;
#endif
    }
}

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
};

static inline slub_slab_t *slub_page_of(void *obj) {
    return (slub_slab_t *)((uint64_t)obj & ~(uint64_t)(PAGE_SIZE - 1));
}

// Lock-free LIFO over the link word stored inside each free object.
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
static inline uint32_t slub_kmalloc_class(size_t size) {
    if (size <= 16) return 0;
    return (uint32_t)(63 - __builtin_clzll((uint64_t)size - 1)) - 3u;
}

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

extern "C" kmem_cache *kmem_cache_create(const char *n, uint64_t sz, uint64_t al) { return SLUB::Create(n, sz, al ? al : 8); }
extern "C" void        kmem_cache_destroy(kmem_cache *c) { SLUB::Destroy(c); }
extern "C" void       *kmem_cache_alloc(kmem_cache *c) { return SLUB::Alloc(c); }
extern "C" void        kmem_cache_free(kmem_cache *c, void *o) { SLUB::Free(c, o); }
#endif // __x86_64__

extern "C" void *kmalloc(uint64_t size) {
#ifdef __x86_64__
    void *p = SLUB::Kmalloc((size_t)size);    // small object: lock-free SLUB path
    if (likely(p)) return p;
#endif
    return SLAB::Alloc(size);                 // >1024 B large object, or SLUB not online
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

extern "C" uint64_t slab_page_pool_count(void) { return g_pool_count; }
extern "C" uint64_t slab_lock_acquires(uint32_t idx) {
    return (idx < MAX_SLAB_ORDER) ? g_cache_lock_acquires[idx] : 0;
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