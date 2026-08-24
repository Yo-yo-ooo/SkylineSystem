//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <klib/klib.h>
#ifdef __x86_64__
#include <arch/x86_64/vmm/vmm.h>
#endif

#define HEAP_MAGIC 0xdead424E //Dead SkylineSystem
#define SLAB_MAGIC (uint32_t)0xdead424E //Dead SkylineSystem


struct slab_page_t;
typedef struct slab_cache_t {
    uint32_t obj_size;
    uint32_t size_class;
    spinlock_t lock;
    void *global_free_list;
    uint64_t global_free_count;
    slab_page_t *partial;
    slab_page_t *full;
    slab_page_t *empty;
    slab_page_t *empty_tail; 
    uint32_t empty_count;
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t cache_hit_count;
    uint64_t total_pages; 
    bool has_redzone;       //  标记该大小类是否启用红区
} slab_cache_t;

/* 
PACK(typedef struct slab_obj_t{
    slab_cache_t *cache;    // 8 bytes
    uint32_t magic;         // 4 bytes
    uint32_t _padding;      // 4 bytes
}) slab_obj_t;
 */
typedef struct slab_page_t{
    uint64_t magic;
    union {
        slab_cache_t *cache;
        uint64_t page_count;
    };
    slab_page_t *next;
    slab_page_t *prev;
    void *freelist;
    uint32_t inuse;
    uint32_t objects;
} slab_page_t;

namespace SLAB{
    void Init();
    void *Alloc(size_t size);
    void *Realloc(void *ptr, size_t size);
    void Free(void *ptr);
    void *UserAlloc(size_t size);

    uint64_t GetSize(void* ptr,bool ERO = false);

    slab_cache_t *GetCache(size_t size);
    slab_cache_t *cache_get_empty(slab_cache_t *cache);
    // FindFree 在新架构下不再需要 O(N) 扫描，保留声明以兼容
    int64_t FindFree(slab_cache_t *cache);
}

extern "C"{
void* kmalloc(uint64_t size);
void kfree(void* ptr);
void* krealloc(void* ptr, uint64_t size);
void *kcalloc(size_t numitems, size_t size);
}
uint64_t GetPtrPointAreaSize(void *ptr);

inline void operator delete(void* p) {kfree(p);}
inline void operator delete(void* ptr, unsigned long){kfree(ptr);}
inline void operator delete[](void* ptr) noexcept {kfree(ptr);}
#include <new>