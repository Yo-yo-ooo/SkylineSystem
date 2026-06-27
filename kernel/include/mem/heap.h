//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <klib/klib.h>
#ifdef __x86_64__
#include <arch/x86_64/vmm/vmm.h>
#endif

#define HEAP_MAGIC 0xdead424E //Dead SkylineSystem
#define SLAB_MAGIC (uint32_t)0xdead424E //Dead SkylineSystem

typedef struct slab_cache_t {
    void *slabs;
    uint64_t obj_size;
    uint64_t free_idx;
    bool used;
    struct slab_cache_t *empty_cache;
} slab_cache_t;

PACK(typedef struct slab_obj_t{
    bool used;
    slab_cache_t *cache;
    uint32_t magic;
}) slab_obj_t;

PACK(typedef struct slab_page_t{
    uint32_t magic;
    uint64_t page_count;
}) slab_page_t;



namespace SLAB{
    void Init();
    void *Alloc(size_t size);
    void *Realloc(void *ptr, size_t size);
    void Free(void *ptr);
    void *UserAlloc(size_t size);

    uint64_t GetSize(void* ptr,bool ERO = false);

    slab_cache_t *GetCache(size_t size);
    slab_cache_t *cache_get_empty(slab_cache_t *cache);
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
#include "new.hpp"