//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <mem/heap.h>
#include <mem/pmm.h>

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>
#endif
#include <pdef.h>

volatile spinlock_t heap_lock = 0;

#define SLAB_PAGES 4
#define SLAB_SIZE (SLAB_PAGES * PAGE_SIZE)

#define SLAB_IDX(cache, i) (slab_obj_t*)((uint64_t)cache->slabs + (i * (sizeof(slab_obj_t) + cache->obj_size)))
#define OBJS_IN_SLAB(cache) (SLAB_SIZE / (cache->obj_size + sizeof(slab_obj_t)))
#define SLAB_BATCH 16

slab_cache_t *caches[8] = { 0 };


//cpu_slab_t* cpu_slabs = nullptr; 

static slab_cache_t *slab_create_cache(uint64_t obj_size, uint32_t size_class) {
#ifdef __x86_64__
    slab_cache_t *cache = (slab_cache_t*)VMM::Alloc(kernel_pagemap, 1, false);
    cache->obj_size = obj_size;
    cache->size_class = size_class;
    uint64_t obj_count = SLAB_SIZE / (cache->obj_size + sizeof(slab_obj_t));
    uint64_t total_size = obj_count * (cache->obj_size + sizeof(slab_obj_t));
    cache->slabs = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(total_size, PAGE_SIZE), false);
    
    cache->global_free_list = nullptr;
    cache->free_idx = 0;
    cache->used = false;
    cache->empty_cache = NULL;
    
    // 预初始化所有对象并挂入 global_free_list
    for (uint64_t i = 0; i < obj_count; i++) {
        slab_obj_t *obj = SLAB_IDX(cache, i);
        obj->cache = cache;
        obj->magic = 0; // 0 表示在链表中空闲
        // 隐式链表：把对象的数据区当作 next 指针用
        *(void**)(obj + 1) = cache->global_free_list;
        cache->global_free_list = (void*)(obj + 1);
    }
    return cache;
#else
    return nullptr;
#endif
}

namespace SLAB{

    void Init() {
#ifdef __x86_64__
        uint64_t last_size = 8;
        uint64_t size = 0;
        for (int32_t i = 0; i < 8; i++) {
            size = last_size * 2;
            last_size = size;
            caches[i] = slab_create_cache(size, i);
        }
        
        
#endif
    }

    slab_cache_t *GetCache(size_t size) {
        if (size <= 16) return caches[0];
        if (size > 2048) return NULL; // 超过 2KB 直接走大页分配
        uint64_t cache = 64 - __builtin_clzll(size - 1);
        cache = (cache >= 4 ? cache - 4 : 0);
        if (cache > 7) return NULL;
        return caches[cache];
    }

    int64_t FindFree(slab_cache_t *cache) {
        return -1; 
    }

    slab_cache_t *cache_get_empty(slab_cache_t *cache) {
        slab_cache_t *empty = cache;
        while (empty->used) {
            if (!empty->empty_cache) {
                empty->empty_cache = slab_create_cache(cache->obj_size, cache->size_class);
                empty = empty->empty_cache;
                break;
            }
            empty = empty->empty_cache;
        }
        return empty;
    }

    void RefillCpuCache(slab_cache_t* cache, uint32_t cpu_id) {
        spinlock_lock(&heap_lock);
        for (int i = 0; i < SLAB_BATCH; i++) {
            if (!cache->global_free_list) {
                if (cache->used && !cache->empty_cache) {
                    cache->empty_cache = slab_create_cache(cache->obj_size, cache->size_class);
                }
                if (cache->empty_cache) {
                    cache = cache->empty_cache;
                } else {
                    cache->used = true;
                    break;
                }
            }
            void* obj = cache->global_free_list;
            cache->global_free_list = *(void**)obj;
            
            cpu_t *cpu = get_cpu(cpu_id);
            *(void**)obj = cpu->cslab.freelist[cache->size_class];
            cpu->cslab.freelist[cache->size_class] = obj;
            cpu->cslab.count[cache->size_class]++;
        }
        spinlock_unlock(&heap_lock);
    }

    void *Alloc(size_t size) {
#ifdef __x86_64__
        slab_cache_t *cache = SLAB::GetCache(size);
        
        if (!cache) {
            uint64_t total_pages = DIV_ROUND_UP(size + sizeof(slab_page_t), PAGE_SIZE);
            slab_page_t *page = (slab_page_t*)VMM::Alloc(kernel_pagemap, total_pages, false);
            page->magic = SLAB_MAGIC;
            page->page_count = total_pages;
            return (void*)(page + 1);
        }

        uint32_t cpu_id = 0;
        cpu_t* cpu = this_cpu();
        if (cpu) cpu_id = cpu->id;

        uint32_t idx = cache->size_class;

        // 快速路径：Per-CPU 无锁分配
        void* obj = cpu->cslab.freelist[idx];
        if (obj) {
            cpu->cslab.freelist[idx] = *(void**)obj;
            cpu->cslab.count[idx]--;
            slab_obj_t* header = (slab_obj_t*)((uint64_t)obj - sizeof(slab_obj_t));
            header->magic = SLAB_MAGIC;
            return obj;
        }

        // 慢速路径：从全局链表批量获取
        RefillCpuCache(cache, cpu_id);
        
        obj = cpu->cslab.freelist[idx];
        if (obj) {
            cpu->cslab.freelist[idx] = *(void**)obj;
            cpu->cslab.count[idx]--;
            slab_obj_t* header = (slab_obj_t*)((uint64_t)obj - sizeof(slab_obj_t));
            header->magic = SLAB_MAGIC;
            return obj;
        }
        
        return nullptr;
#endif
    }

    void *Realloc(void *ptr, size_t size) {
#ifdef __x86_64__
        if (!ptr) return SLAB::Alloc(size);
        if (size == 0){SLAB::Free(ptr);return nullptr;}
        
        uint64_t current_capacity = SLAB::GetSize(ptr, true);
        if (size <= current_capacity) {
            if (size >= (current_capacity / 4) || current_capacity <= 64) {
                return ptr; 
            }
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

        slab_page_t *page = (slab_page_t*)((uint64_t)ptr - sizeof(slab_page_t));
        if (page->magic == SLAB_MAGIC) {
            spinlock_lock(&heap_lock);
            VMM::Free(kernel_pagemap, page);
            spinlock_unlock(&heap_lock);
            return;
        }

        slab_obj_t *obj = (slab_obj_t*)((uint64_t)ptr - sizeof(slab_obj_t));
        if (obj->magic != SLAB_MAGIC) {
            kerror("Critical SLAB error: Trying to free invalid ptr.\n");
            return;
        }
        
        slab_cache_t *cache = obj->cache;
        uint32_t idx = cache->size_class;

        uint32_t cpu_id = 0;
        cpu_t* cpu = this_cpu();
        if (cpu) cpu_id = cpu->id;

        obj->magic = 0; // 标记为空闲
        
        // 直接挂入当前 CPU 的 Per-CPU 链表 (无锁释放)
        *(void**)ptr = cpu->cslab.freelist[idx];
        cpu->cslab.freelist[idx] = ptr;
        cpu->cslab.count[idx]++;

        // 缓存积压过多，批量归还给全局链表
        if (cpu->cslab.count[idx] > SLAB_BATCH * 2) {
            spinlock_lock(&heap_lock);
            for (int i = 0; i < SLAB_BATCH; i++) {
                void* free_obj = cpu->cslab.freelist[idx];
                if (!free_obj) break;
                cpu->cslab.freelist[idx] = *(void**)free_obj;
                cpu->cslab.count[idx]--;
                
                *(void**)free_obj = cache->global_free_list;
                cache->global_free_list = free_obj;
            }
            spinlock_unlock(&heap_lock);
        }
#endif
    }

    void *UserAlloc(size_t size) {
        return SLAB::Alloc(size); 
    }

    uint64_t GetSize(void* ptr, bool ERO) {
#ifdef __x86_64__
        if (!ptr || (uint64_t)ptr < 0x1000) return 0;
        slab_page_t *page = (slab_page_t*)((uint64_t)ptr - sizeof(slab_page_t));
        if (page->magic == SLAB_MAGIC) {
            return page->page_count * PAGE_SIZE - sizeof(slab_page_t);
        } 
        slab_obj_t *obj = (slab_obj_t*)((uint64_t)ptr - sizeof(slab_obj_t));
        if (obj->magic == SLAB_MAGIC) {
            if (obj->cache) return obj->cache->obj_size;
        }
        if (!ERO) kerror("Invalid SLAB ptr\n");
        return (ERO ? 1 : UINT64_MAX);
#else
        return 0;
#endif
    }
}

extern "C" void* kmalloc(uint64_t size) { return SLAB::Alloc(size); }
extern "C" void kfree(void *ptr) { SLAB::Free(ptr); }
extern "C" void* krealloc(void *ptr, uint64_t size) { return SLAB::Realloc(ptr,size); }

uint64_t GetPtrPointAreaSize(void *ptr){ return SLAB::GetSize(ptr); }

#define align4(x) (((((x)-1)>>2)<<2)+4)
extern "C" void *kcalloc(size_t numitems, size_t size) {
    size_t *knew;
    size_t s, i;
    knew = kmalloc(numitems * size);
    if(knew) {
        s = align4(numitems * size) >> 2;
        _memset(knew,0,s);
    }
    return knew;
}