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

//#define MAX_SLAB_ORDER 7
#define MAX_SLAB_SIZE 1024

// Per-CPU 水位线调优：当前回流阈值为 2 * SLAB_BATCH。
#define SLAB_BATCH 16
#define DRAIN_HIGH_WATERMARK (SLAB_BATCH * 4)
#define DRAIN_LOW_WATERMARK (SLAB_BATCH * 2)
#define EMPTY_CACHE_LIMIT 4

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

static slab_page_t *slab_alloc_page(slab_cache_t *cache) {
#ifdef __x86_64__
    slab_page_t *page = (slab_page_t*)VMM::Alloc(kernel_pagemap, SLAB_PAGES, false);

    if (!page) return nullptr;
    
    page->magic = SLAB_PAGE_MAGIC;
    page->cache = cache;
    page->next = page->prev = nullptr;
    page->inuse = 0;
    
    uint64_t header_size = SLAB_ALIGN_UP(sizeof(slab_page_t), cache->obj_size);
    char *ptr = (char*)page + header_size;
    
    uint64_t avail = SLAB_SIZE - header_size;
    page->objects = avail / cache->obj_size;
    if (page->objects == 0) {
        VMM::Free(kernel_pagemap, page);
        return nullptr;
    }
    
    page->freelist = ptr;
    for (uint32_t i = 0; i < page->objects; i++) {
        if (i == page->objects - 1) {
            *(void**)ptr = nullptr;
        } else {
            *(void**)ptr = ptr + cache->obj_size;
        }
        
#ifdef SLAB_DEBUG_POISON
        // 优化：仅大对象初始化红区魔数
        if (cache->has_redzone) {
            *(uint64_t*)(ptr + cache->obj_size - REDZONE_SIZE) = SLAB_REDZONE_MAGIC;
        }
#endif
        ptr += cache->obj_size;
    }
    
    __atomic_add_fetch(&cache->total_pages, 1, __ATOMIC_RELAXED);
    return page;
#else
    return nullptr;
#endif
}

// 优化：在已持有 cache->lock 的状态下执行，返回需要释放的页链表
// 契约：调用此函数前必须持有 cache->lock！
static slab_page_t *DrainGlobalFreeListLocked(slab_cache_t *cache, uint64_t flags) {
    if (cache->global_free_count < DRAIN_HIGH_WATERMARK) {
        return nullptr;
    }
    
    uint64_t to_drain = cache->global_free_count - DRAIN_LOW_WATERMARK;
    slab_page_t *pages_to_free = nullptr;

    for (uint64_t i = 0; i < to_drain; i++) {
        void *obj = cache->global_free_list;
        if (!obj) break;
        
        cache->global_free_list = *(void**)obj;
        cache->global_free_count--;
        
        slab_page_t *page = (slab_page_t*)((uint64_t)obj & ~(PAGE_SIZE - 1));

        // 快速路径 Double-Free 检测取舍：
        // 此处检测对象归还时的 inuse 下溢，配合毒化与红区机制已能暴露绝大多数内存错误。
        if (page->inuse == 0) {
            spin_unlock_irqrestore(&cache->lock, flags);
            Panic("SLAB error: inuse underflow on drain (double free detected)\n");
        }

        *(void**)obj = page->freelist;
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
                else cache->empty_tail = nullptr;
                cache->empty_count--;

                p_to_free->next = pages_to_free;
                p_to_free->prev = nullptr;
            }
        }
    }
    return pages_to_free;
}

namespace SLAB {

    void Init() {
#ifdef __x86_64__
        uint32_t base_size = 16;
        for (uint32_t i = 0; i < MAX_SLAB_ORDER; i++) {
            caches[i].size_class = i;
            
            // 优化：小对象红区空间开销过高，仅对象 >= 64 时启用红区
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
        for (uint32_t i = 0; i < MAX_SLAB_ORDER; i++) {
            uint64_t flags = irq_save();
            void* batch_to_drain[SLAB_BATCH];
            int drain_count = 0;
            
            while (cpu->cslab.count[i] > 0) {
                batch_to_drain[drain_count] = cpu->cslab.freelist[i];
                cpu->cslab.freelist[i] = *(void**)batch_to_drain[drain_count];
                cpu->cslab.count[i]--;
                drain_count++;
                
                if (drain_count == SLAB_BATCH) {
                    irq_restore(flags);
                    uint64_t flags2 = spin_lock_irqsave(&caches[i].lock);
                    for (int j = 0; j < drain_count; j++) {
                        *(void**)batch_to_drain[j] = caches[i].global_free_list;
                        caches[i].global_free_list = batch_to_drain[j];
                        caches[i].global_free_count++;
                    }
                    slab_page_t *pages_to_free = DrainGlobalFreeListLocked(&caches[i], flags2);
                    spin_unlock_irqrestore(&caches[i].lock, flags2);
                    while (pages_to_free) {
                        slab_page_t *p = pages_to_free;
                        pages_to_free = p->next;
                        __atomic_sub_fetch(&caches[i].total_pages, 1, __ATOMIC_RELAXED);
                        VMM::Free(kernel_pagemap, p);
                    }
                    drain_count = 0;
                    flags = irq_save();
                }
            }
            
            if (drain_count > 0) {
                irq_restore(flags);
                uint64_t flags2 = spin_lock_irqsave(&caches[i].lock);
                for (int j = 0; j < drain_count; j++) {
                    *(void**)batch_to_drain[j] = caches[i].global_free_list;
                    caches[i].global_free_list = batch_to_drain[j];
                    caches[i].global_free_count++;
                }
                slab_page_t *pages_to_free = DrainGlobalFreeListLocked(&caches[i], flags2);
                spin_unlock_irqrestore(&caches[i].lock, flags2);
                while (pages_to_free) {
                    slab_page_t *p = pages_to_free;
                    pages_to_free = p->next;
                    __atomic_sub_fetch(&caches[i].total_pages, 1, __ATOMIC_RELAXED);
                    VMM::Free(kernel_pagemap, p);
                }
            } else {
                irq_restore(flags);
            }
        }
    }

    // 优化：移除冗余的零值处理，Init 已叠加红区开销，此处直接匹配
    slab_cache_t *GetCache(size_t size) {
        if (size > MAX_SLAB_SIZE) return nullptr;
        if (size <= 16) return &caches[0];
        
        uint32_t bits = 63 - __builtin_clzll((uint64_t)size - 1);
        uint32_t idx = bits - 3; 
        
        if (idx >= MAX_SLAB_ORDER) return nullptr;
        return &caches[idx];
    }

    // 新增：运行时红区主动校验接口
    void CheckRedzone(void *ptr) {
#ifdef SLAB_DEBUG_POISON
        if (!ptr) return;
        slab_page_t *page = (slab_page_t*)((uint64_t)ptr & ~(PAGE_SIZE - 1));
        
        if (page->magic == SLAB_PAGE_MAGIC) {
            slab_cache_t *cache = page->cache;
            if (cache->has_redzone) {
                if (*(uint64_t*)((uint64_t)ptr + cache->obj_size - REDZONE_SIZE) != SLAB_REDZONE_MAGIC) {
                    Panic("SLAB error: Redzone corrupted!\n");
                }
            }
        } else if (page->magic == LARGE_PAGE_MAGIC) {
            uint64_t usable_size = page->page_count * PAGE_SIZE - sizeof(slab_page_t) - REDZONE_SIZE;
            if (*(uint64_t*)((uint64_t)ptr + usable_size) != SLAB_REDZONE_MAGIC) {
                Panic("Critical SLAB error: Large page redzone corrupted!\n");
            }
        }
#endif
    }

    void *Alloc(size_t size) {
#ifdef __x86_64__
        if (size == 0) size = 1;
        
        if (size > MAX_SLAB_SIZE) {
            uint64_t total_size;
            if (__builtin_add_overflow(size, sizeof(slab_page_t) + REDZONE_SIZE, &total_size)) {
                return nullptr; 
            }
            
            uint64_t pages = DIV_ROUND_UP(total_size, PAGE_SIZE);
            slab_page_t *page = (slab_page_t*)VMM::Alloc(kernel_pagemap, pages, false);

            if (!page) return nullptr;
            
            page->magic = LARGE_PAGE_MAGIC;
            page->page_count = pages;
            
#ifdef SLAB_DEBUG_POISON
            // 大页分配统一启用红区
            uint64_t usable_size = pages * PAGE_SIZE - sizeof(slab_page_t) - REDZONE_SIZE;
            if (usable_size > 0) {
                _memset((void*)(page + 1), SLAB_POISON_ALLOC, usable_size);
                *(uint64_t*)((uint64_t)(page + 1) + usable_size) = SLAB_REDZONE_MAGIC;
            }
#endif
            return (void*)(page + 1);
        }
        
        slab_cache_t *cache = GetCache(size);
        if (!cache) return nullptr;
        
        uint32_t idx = cache->size_class;
        __atomic_add_fetch(&cache->alloc_count, 1, __ATOMIC_RELAXED);
        
        uint64_t flags = irq_save();
        cpu_t* cpu = this_cpu();
        
        if (cpu && cpu->cslab.count[idx] > 0) {
            void* obj = cpu->cslab.freelist[idx];
            cpu->cslab.freelist[idx] = *(void**)obj;
            cpu->cslab.count[idx]--;
            irq_restore(flags);
            __atomic_add_fetch(&cache->cache_hit_count, 1, __ATOMIC_RELAXED);
            
#ifdef SLAB_DEBUG_POISON
            uint64_t usable_size = cache->obj_size - (cache->has_redzone ? REDZONE_SIZE : 0);
            if (cache->has_redzone) {
                if (*(uint64_t*)((uint64_t)obj + cache->obj_size - REDZONE_SIZE) != SLAB_REDZONE_MAGIC) {
                    Panic("SLAB error: Buffer overflow detected on alloc!\n");
                }
            }
            if (usable_size > 8) {
                _memset((void*)((uint64_t)obj + 8), SLAB_POISON_ALLOC, usable_size - 8);
            }
#endif
            return obj; 
        }
        irq_restore(flags);

        void* batch[SLAB_BATCH];
        int batch_cnt = 0;
        int target_cnt = cpu ? SLAB_BATCH : 1;

        flags = spin_lock_irqsave(&cache->lock);
        
        for (int i = 0; i < target_cnt; i++) {
            if (cache->global_free_list) {
                batch[i] = cache->global_free_list;
                cache->global_free_list = *(void**)batch[i];
                cache->global_free_count--;
            } else {
                if (!cache->partial) {
                    if (cache->empty) {
                        cache->partial = cache->empty;
                        cache->empty = cache->partial->next;
                        if (cache->empty) cache->empty->prev = nullptr;
                        else cache->empty_tail = nullptr;
                        cache->empty_count--;
                        cache->partial->next = nullptr;
                        cache->partial->prev = nullptr;
                    } else {
                        cache->partial = slab_alloc_page(cache);
                        if (!cache->partial) break; 
                    }
                }
                slab_page_t *page = cache->partial;
                batch[i] = page->freelist;
                page->freelist = *(void**)batch[i];
                
                if (page->inuse >= page->objects) {
                    spin_unlock_irqrestore(&cache->lock, flags);
                    Panic("SLAB error: inuse overflow on alloc\n");
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
                *(void**)batch[i] = cpu->cslab.freelist[idx];
                cpu->cslab.freelist[idx] = batch[i];
                cpu->cslab.count[idx]++;
            }
            spin_unlock_irqrestore(&cache->lock, flags);
            
#ifdef SLAB_DEBUG_POISON
            uint64_t usable_size = cache->obj_size - (cache->has_redzone ? REDZONE_SIZE : 0);
            if (cache->has_redzone) {
                if (*(uint64_t*)((uint64_t)batch[0] + cache->obj_size - REDZONE_SIZE) != SLAB_REDZONE_MAGIC) {
                    Panic("SLAB error: Buffer overflow detected on slow alloc!\n");
                }
            }
            if (usable_size > 8) {
                _memset((void*)((uint64_t)batch[0] + 8), SLAB_POISON_ALLOC, usable_size - 8);
            }
#endif
            return batch[0];
        }
        
        for (int i = 1; i < batch_cnt; i++) {
            *(void**)batch[i] = cache->global_free_list;
            cache->global_free_list = batch[i];
            cache->global_free_count++;
        }
        spin_unlock_irqrestore(&cache->lock, flags);

#ifdef SLAB_DEBUG_POISON
        uint64_t usable_size_fb = cache->obj_size - (cache->has_redzone ? REDZONE_SIZE : 0);
        if (cache->has_redzone) {
            if (*(uint64_t*)((uint64_t)batch[0] + cache->obj_size - REDZONE_SIZE) != SLAB_REDZONE_MAGIC) {
                Panic("SLAB error: Buffer overflow detected on fallback alloc!\n");
            }
        }
        if (usable_size_fb > 8) {
            _memset((void*)((uint64_t)batch[0] + 8), SLAB_POISON_ALLOC, usable_size_fb - 8);
        }
#endif
        return batch[0];
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

        slab_page_t *page = (slab_page_t*)((uint64_t)ptr & ~(PAGE_SIZE - 1));
        
        if (page->magic == LARGE_PAGE_MAGIC) {
            if ((void*)(page + 1) != ptr) {
                Panic("Critical SLAB error: Freeing non-original large page ptr.\n");
            }

#ifdef SLAB_DEBUG_POISON
            uint64_t usable_size = page->page_count * PAGE_SIZE - sizeof(slab_page_t) - REDZONE_SIZE;
            if (usable_size > 0) {
                if (*(uint64_t*)((uint64_t)ptr + usable_size) != SLAB_REDZONE_MAGIC) {
                    Panic("Critical SLAB error: Large page buffer overflow detected on free!\n");
                }
                _memset(ptr, SLAB_POISON_FREE, usable_size);
            }
#endif
            page->magic = 0;
            VMM::Free(kernel_pagemap, page);
            return;
        }
        
        if (page->magic != SLAB_PAGE_MAGIC) {
            Panic("Critical SLAB error: Trying to free invalid or corrupted ptr.\n");
        }
        
        slab_cache_t *cache = page->cache;
        __atomic_add_fetch(&cache->free_count, 1, __ATOMIC_RELAXED);
        uint32_t idx = cache->size_class;

        uint64_t header_size = SLAB_ALIGN_UP(sizeof(slab_page_t), cache->obj_size);
        uintptr_t offset = (uintptr_t)ptr - (uintptr_t)page;
        if (offset < header_size || (offset - header_size) % cache->obj_size != 0) {
            Panic("SLAB error: Freeing non-object-aligned pointer.\n");
        }

        uint64_t flags = irq_save();
        cpu_t* cpu = this_cpu();
        
        if (cpu) {
#ifdef SLAB_DEBUG_POISON
            uint64_t usable_size = cache->obj_size - (cache->has_redzone ? REDZONE_SIZE : 0);
            if (cache->has_redzone) {
                if (*(uint64_t*)((uint64_t)ptr + cache->obj_size - REDZONE_SIZE) != SLAB_REDZONE_MAGIC) {
                    Panic("SLAB error: Buffer overflow detected on free!\n");
                }
            }
            if (usable_size > 8) {
                _memset((void*)((uint64_t)ptr + 8), SLAB_POISON_FREE, usable_size - 8);
            }
#endif
            *(void**)ptr = cpu->cslab.freelist[idx];
            cpu->cslab.freelist[idx] = ptr;
            cpu->cslab.count[idx]++;

            if (cpu->cslab.count[idx] > SLAB_BATCH * 2) {
                void* batch_to_drain[SLAB_BATCH];
                int drain_count = 0;
                
                for (int i = 0; i < SLAB_BATCH; i++) {
                    batch_to_drain[i] = cpu->cslab.freelist[idx];
                    if (!batch_to_drain[i]) break;
                    cpu->cslab.freelist[idx] = *(void**)batch_to_drain[i];
                    cpu->cslab.count[idx]--;
                    drain_count++;
                }
                irq_restore(flags); 
                
                uint64_t flags2 = spin_lock_irqsave(&cache->lock);
                for (int i = 0; i < drain_count; i++) {
                    *(void**)batch_to_drain[i] = cache->global_free_list;
                    cache->global_free_list = batch_to_drain[i];
                    cache->global_free_count++;
                }
                slab_page_t *pages_to_free = DrainGlobalFreeListLocked(cache, flags2);
                spin_unlock_irqrestore(&cache->lock, flags2);

                while (pages_to_free) {
                    slab_page_t *p = pages_to_free;
                    pages_to_free = p->next;
                    __atomic_sub_fetch(&cache->total_pages, 1, __ATOMIC_RELAXED);
                    VMM::Free(kernel_pagemap, p);
                }
                return;
            }
            irq_restore(flags);
        } else {
            irq_restore(flags);
            uint64_t flags2 = spin_lock_irqsave(&cache->lock);
            *(void**)ptr = cache->global_free_list;
            cache->global_free_list = ptr;
            cache->global_free_count++;
            slab_page_t *pages_to_free = DrainGlobalFreeListLocked(cache, flags2);
            spin_unlock_irqrestore(&cache->lock, flags2);

            while (pages_to_free) {
                slab_page_t *p = pages_to_free;
                pages_to_free = p->next;
                __atomic_sub_fetch(&cache->total_pages, 1, __ATOMIC_RELAXED);
                VMM::Free(kernel_pagemap, p);
            }
        }
#endif
    }

    void *UserAlloc(size_t size) {
        return SLAB::Alloc(size); 
    }

    uint64_t GetSize(void* ptr, bool ERO) {
#ifdef __x86_64__
        if (!ptr || (uint64_t)ptr < 0x1000) return 0;
        
        slab_page_t *page = (slab_page_t*)((uint64_t)ptr & ~(PAGE_SIZE - 1));
        
        if (page->magic == LARGE_PAGE_MAGIC) {
            return page->page_count * PAGE_SIZE - sizeof(slab_page_t) - REDZONE_SIZE;
        } 
        if (page->magic == SLAB_PAGE_MAGIC) {
            if (page->cache) {
                return page->cache->obj_size - (page->cache->has_redzone ? REDZONE_SIZE : 0);
            }
        }
        
        if (!ERO) Panic("Invalid SLAB ptr in GetSize\n");
        return 0; 
#else
        return 0;
#endif
    }
}

extern "C" void* kmalloc(uint64_t size) { return SLAB::Alloc(size); }
extern "C" void kfree(void *ptr) { SLAB::Free(ptr); }
extern "C" void* krealloc(void *ptr, uint64_t size) { return SLAB::Realloc(ptr,size); }

uint64_t GetPtrPointAreaSize(void *ptr){ return SLAB::GetSize(ptr, false); }

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