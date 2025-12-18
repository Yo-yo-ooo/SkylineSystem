#include <mem/heap.h>
#include <mem/pmm.h>

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>
#endif
#include <pdef.h>

volatile spinlock_t heap_lock = 0;


#define SLAB_PAGES 4
#define SLAB_SIZE (SLAB_PAGES * PAGE_SIZE)

#define SLAB_IDX(cache, i) (slab_obj_t*)(cache->slabs + (i * (sizeof(slab_obj_t) + cache->obj_size)))
#define OBJS_IN_SLAB(cache) (SLAB_SIZE / (cache->obj_size + sizeof(slab_obj_t)))

// Only 8 because once we hit 4096, the VMA is going to take over.
slab_cache_t *caches[8] = { 0 };

static slab_cache_t *slab_create_cache(uint64_t obj_size) {
#ifdef __x86_64__
    slab_cache_t *cache = (slab_cache_t*)VMM::Alloc(kernel_pagemap, 1, false);
    cache->obj_size = obj_size;
    uint64_t obj_count = SLAB_SIZE / (cache->obj_size + sizeof(slab_obj_t));
    uint64_t total_size = obj_count * (cache->obj_size + sizeof(slab_obj_t));
    cache->slabs = VMM::Alloc(kernel_pagemap, DIV_ROUND_UP(total_size, PAGE_SIZE), false);
    cache->free_idx = 0;
    cache->used = false;
    cache->empty_cache = NULL;
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
            caches[i] = slab_create_cache(size);
        }
#endif
    }

    slab_cache_t *GetCache(size_t size) {
        uint64_t cache = 64 - __builtin_clzll(size - 1);
        cache = (cache >= 4 ? cache - 4 : 0);
        if (cache > 7)
            return NULL;
        return caches[cache];
    }

    int64_t FindFree(slab_cache_t *cache) {
        if (cache->free_idx < OBJS_IN_SLAB(cache))
            return cache->free_idx++;
        for (uint64_t i = 0; i < OBJS_IN_SLAB(cache); i++) {
            slab_obj_t *obj = SLAB_IDX(cache, i);
            if (obj->magic != SLAB_MAGIC)
                return i;
        }
        return -1;
    }

    slab_cache_t *cache_get_empty(slab_cache_t *cache) {
        slab_cache_t *empty = cache;
        while (empty->used) {
            if (!empty->empty_cache) {
                empty->empty_cache = slab_create_cache(cache->obj_size);
                empty = empty->empty_cache;
                break;
            }
            empty = empty->empty_cache;
        }
        return empty;
    }

    void *Alloc(size_t size) {
#ifdef __x86_64__
        spinlock_lock(&heap_lock);
        slab_cache_t *cache = SLAB::GetCache(size);
        if (!cache) {
            uint64_t total_pages = DIV_ROUND_UP(size + sizeof(slab_page_t), PAGE_SIZE);
            slab_page_t *page = VMM::Alloc(kernel_pagemap, total_pages, false);
            page->magic = SLAB_MAGIC;
            page->page_count = total_pages;
            spinlock_unlock(&heap_lock);
            return (void*)(page + 1);
        }
        bool found = false;
        int64_t free_obj;
        slab_obj_t *obj = NULL;
        while (!found) {
            if (cache->used) {
                cache = cache_get_empty(cache);
            }
            free_obj = SLAB::FindFree(cache);
            if (free_obj == -1)
                cache->used = true;
            else {
                obj = SLAB_IDX(cache, free_obj);
                found = true;
            }
        }
        obj->cache = cache;
        obj->magic = SLAB_MAGIC;
        spinlock_unlock(&heap_lock);
        return (void*)(obj + 1);
#endif
    }

    void *Realloc(void *ptr, size_t size) {
#ifdef __x86_64__
        if (!ptr) return SLAB::Alloc(size);
        uint64_t old_size = 0;
        slab_page_t *page = (slab_page_t*)((uint64_t)ptr - sizeof(slab_page_t));
        uint64_t dbg = (uint64_t)page;
        if (page->magic != SLAB_MAGIC) {
            slab_obj_t *obj = (slab_obj_t*)((uint64_t)ptr - sizeof(slab_obj_t));
            if (obj->magic != SLAB_MAGIC) {
                kerror("Critical SLAB error: Trying to reallocate invalid ptr.\n");
                ASSERT(0);
                return nullptr;
            }
            old_size = obj->cache->obj_size;
            dbg = (uint64_t)obj;
        } else {
            old_size = page->page_count * PAGE_SIZE - sizeof(slab_page_t);
        }
        void *new_ptr = SLAB::Alloc(size);
        __memcpy(new_ptr, ptr, (old_size > size ? size : old_size));
        SLAB::Free(ptr);
        return new_ptr;
#endif
    }

    void Free(void *ptr) {
#ifdef __x86_64__
        spinlock_lock(&heap_lock);
        slab_page_t *page = (slab_page_t*)((uint64_t)ptr - sizeof(slab_page_t));
        if (page->magic != SLAB_MAGIC) {
            slab_obj_t *obj = (slab_obj_t*)((uint64_t)ptr - sizeof(slab_obj_t));
            if (obj->magic != SLAB_MAGIC) {
                kerror("Critical SLAB error: Trying to free invalid ptr.\n");
                spinlock_unlock(&heap_lock);
                return;
            }
            slab_cache_t *cache = obj->cache;
            if (cache->used)
                cache->used = false;
            obj->magic = 0;
            spinlock_unlock(&heap_lock);
            return;
        }
        VMM::Free(kernel_pagemap, page);
        spinlock_unlock(&heap_lock);
#endif
    }

    void *UserAlloc(size_t size) {
#ifdef __x86_64__
        spinlock_lock(&heap_lock);
        slab_cache_t *cache = SLAB::GetCache(size);
        if (!cache) {
            uint64_t total_pages = DIV_ROUND_UP(size + sizeof(slab_page_t), PAGE_SIZE);
            slab_page_t *page = VMM::Alloc(kernel_pagemap, total_pages, true);
            page->magic = SLAB_MAGIC;
            page->page_count = total_pages;
            spinlock_unlock(&heap_lock);
            return (void*)(page + 1);
        }
        bool found = false;
        int64_t free_obj;
        slab_obj_t *obj = NULL;
        while (!found) {
            if (cache->used) {
                cache = cache_get_empty(cache);
            }
            free_obj = SLAB::FindFree(cache);
            if (free_obj == -1)
                cache->used = true;
            else {
                obj = SLAB_IDX(cache, free_obj);
                found = true;
            }
        }
        obj->cache = cache;
        obj->magic = SLAB_MAGIC;
        spinlock_unlock(&heap_lock);
        return (void*)(obj + 1);
#endif
    }

    uint64_t GetSize(void* ptr,bool ERO = false){
#ifdef __x86_64__
        spinlock_lock(&heap_lock);
        slab_page_t *page = (slab_page_t*)((uint64_t)ptr - sizeof(slab_page_t));
        if (page->magic != SLAB_MAGIC) {
            slab_obj_t *obj = (slab_obj_t*)((uint64_t)ptr - sizeof(slab_obj_t));
            if (obj->magic != SLAB_MAGIC) {
                if(ERO == false){
                    kerror("Critical SLAB error: Trying to get size of ptr.\n");
                    return UINT64_MAX;
                }else{return 1;}
            }
            spinlock_unlock(&heap_lock);
            return obj->cache->obj_size;
        }else{
            return page->page_count * PAGE_SIZE - sizeof(slab_page_t);
        }
        spinlock_unlock(&heap_lock);
#endif
    }
}

void* kmalloc(u64 size) {
    return SLAB::Alloc(size);
}

void kfree(void* ptr) {
    SLAB::Free(ptr);
}

void* krealloc(void* ptr, u64 size) {
    return SLAB::Realloc(ptr,size);
}

uint64_t GetPtrPointAreaSize(void *ptr){
    return SLAB::GetSize(ptr);
}


#define align4(x) (((((x)-1)>>2)<<2)+4)
void *kcalloc(size_t numitems, size_t size)
{
    size_t *knew;
    size_t s, i;
    knew = kmalloc(numitems * size);
    if(knew)
    {
        s = align4(numitems * size) >> 2;
        _memset(knew,0,s);
    }
    return knew;
}

