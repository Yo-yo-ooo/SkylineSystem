//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#ifndef _VMM_H_
#define _VMM_H_

#include <stdint.h>
#include <stddef.h>
#include <arch/x86_64/interrupt/idt.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 1

#define MM_READ 1
#define MM_WRITE 2
#define MM_USER 4
#define MM_NX (1ull << 63)

#define PAGE_SIZE 4096ULL
#define PAGE_2MB (2ULL * 1024 * 1024)
#define PAGE_1GB (1ULL * 1024 * 1024 * 1024)
#define PAGE_2GB (2ULL * PAGE_1GB)

#define VMM_FLAG_PRESENT        (1 << 0)        /* P   */
#define VMM_FLAG_READWRITE      (1 << 1)        /* R/W */
#define VMM_FLAG_USER           (1 << 2)        /* U/S */
#define VMM_FLAG_WRITETHROUGH   (1 << 3)        /* PWT */
#define VMM_FLAG_CACHE_DISABLE  (1 << 4)        /* PCD */
#define VMM_FLAG_PAT            (1 << 7)        /* PAT */

// x86_64 巨页 (Huge Page) 的 PS (Page Size) 位
#define VMM_PS_BIT              (1ULL << 7)

#define PTE_MMIO (1 << 9)

#define VMM_FLAGS_DEFAULT       (VMM_FLAG_PRESENT | VMM_FLAG_READWRITE)

#define VMM_FLAGS_MMIO          (VMM_FLAGS_DEFAULT | VMM_FLAG_CACHE_DISABLE \
                                | VMM_FLAG_WRITETHROUGH)
#define VMM_FLAGS_USERMODE      (VMM_FLAGS_DEFAULT | VMM_FLAG_USER)

#define PML5E(x) ((x >> 48) & 0x1ff)
#define PML4E(x) ((x >> 39) & 0x1ff)
#define PDPTE(x) ((x >> 30) & 0x1ff)
#define PDE(x)  ((x >> 21) & 0x1ff)
#define PTE(x)  ((x >> 12) & 0x1ff)

#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_FLAGS_MASK 0x8000000000000FFFULL 

#define PTE_MASK(x) (typeof(x))((uint64_t)x & PTE_ADDR_MASK)
#define PTE_FLAGS(x) (typeof(x))((uint64_t)x & PTE_FLAGS_MASK)

#define PAGE_EXISTS(x) ((uint64_t)x & MM_READ)

static inline bool is_user_address(uint64_t addr){
    return __builtin_expect(addr < 0xFFFF800000000000, 1);
}

static inline bool is_user_buffer_valid(uint64_t addr, size_t count) {
    if (addr > addr + count) return false; 
    return (addr + count) <= 0xFFFF800000000000;
}

typedef struct vma_region_t {
    uint64_t start;
    uint64_t page_count;
    uint64_t flags;
    struct vma_region_t *next;
    struct vma_region_t *prev;
} vma_region_t;

typedef struct vm_mapping_t {
    uint64_t start;
    uint64_t page_count;
    uint64_t flags;
    struct vm_mapping_t *next;
    struct vm_mapping_t *prev;
} vm_mapping_t;

typedef struct {
    volatile uint64_t *toplvl;
    vm_mapping_t *vm_mappings;
    int32_t vma_lock;
    vma_region_t *vma_head;
    vma_region_t *vma_cursor; // 游标：用于加速分配和释放定位
} pagemap_t;

extern volatile pagemap_t *kernel_pagemap;

#define USER_SPACE_END_4LVL 0x00007FFFFFFFFFFF
#define USER_SPACE_END_5LVL 0x00FEFFFFFFFFFFFF
#define PAGE_MASK      0xFFFULL

namespace VMM{
    
    namespace UserAccess {
        bool CopyToUser(pagemap_t* pagemap, uint64_t u_dest, const void* k_src, uint64_t len);
        bool CopyFromUser(pagemap_t* pagemap, void* k_dest, const void* u_src, uint64_t len);
    }

    namespace Useless {
        struct PageInfo {
            uint64_t phys;
            uint64_t size;
            uint64_t flags;
        };

        uint64_t *NewLevel(uint64_t *level, uint64_t entry);
        PageInfo GetPageInfo(pagemap_t *pagemap, uint64_t vaddr);
        
        // 保持兼容，调用 VMA::InternalAlloc
        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags);
    } // namespace Useless

    extern "C" void Init();
    
    void Map(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags);
    void Map(uint64_t vaddr, uint64_t paddr);

    void Map4K(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags);
    void Map2M(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags);
    void Map1G(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags);

    void Unmap(pagemap_t *pagemap, uint64_t vaddr);
    uint64_t GetPhysics(pagemap_t *pagemap, uint64_t vaddr);
    void MapRange(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags, uint64_t count);
    pagemap_t *SwitchPageMap(pagemap_t *pagemap);
    pagemap_t *NewPM();

    namespace VMA{
        void SetStart(pagemap_t *pagemap, uint64_t start, uint64_t page_count);
        vma_region_t *AddRegion(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags);
        vma_region_t *InsertRegion(vma_region_t *after, uint64_t start, uint64_t page_count, uint64_t flags);
        void RemoveRegion(vma_region_t *region);
        
        // 新增：优化后的查找和分配接口
        vma_region_t *FindRegion(pagemap_t *pagemap, uint64_t addr);
        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags, uint64_t hint = 0);
    }

    vm_mapping_t *NewMapping(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags);
    void RemoveMapping(vm_mapping_t *mapping);

    void *Alloc(pagemap_t *pagemap, uint64_t page_count, bool user);
    void *EAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags);
    void Free(pagemap_t *pagemap, void *ptr);

    pagemap_t *Fork(pagemap_t *parent);
    void CleanPM(pagemap_t *pagemap);
    void DestroyPM(pagemap_t *pagemap);

    uint32_t HandlePF(context_t *ctx);
}

#endif