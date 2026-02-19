/*
* SPDX-License-Identifier: GPL-2.0-only
* File: heap.h
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#pragma once

#include <klib/klib.h>
#ifdef __x86_64__
#include <arch/x86_64/vmm/vmm.h>
#endif

#define HEAP_MAGIC 0xdeadbeef
#define SLAB_MAGIC (uint32_t)0xdeadbeef

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