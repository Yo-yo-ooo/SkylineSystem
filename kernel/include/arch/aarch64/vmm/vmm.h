//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#ifndef _AARCH64_VMM_H
#define _AARCH64_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <arch/aarch64/vmm/plvldescs.h>

// ==================== 跨架构统一的权限标志位 ====================
#define MM_READ  (1ULL << 0)
#define MM_WRITE (1ULL << 1)
#define MM_USER  (1ULL << 2)
#define MM_NX    (1ULL << 3) // No Execute

// ==================== 页面大小常量 ====================
#define PAGE_SIZE 4096
#define PAGE_2MB  (512 * PAGE_SIZE)
#define PAGE_1GB  (512 * PAGE_2MB)
#define PAGE_2GB  (2 * PAGE_1GB)

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
    // AArch64 需要双根页表: toplvl[0] = TTBR0 (User), toplvl[1] = TTBR1 (Kernel)
    // 在 x86_64 中可以只用 toplvl[0]
    volatile uint64_t *toplvl[2];
    vm_mapping_t *vm_mappings;
    volatile int32_t vma_lock;
    vma_region_t *vma_head;
} pagemap_t;

// 页表项查询返回的结构体

enum page_size {
    Size4KiB,
    Size2MiB,
    Size1GiB
};

namespace VMM
{
    extern volatile pagemap_t *kernel_pagemap;
    extern volatile bool IsPM5LVL;

    void Init();
    void DetectPagingMode();
    
    pagemap_t *SwitchPageMap(pagemap_t *pagemap);
    pagemap_t *NewPM();
    void DestroyPM(pagemap_t *pagemap);

    // 映射与解除映射
    void Map4K(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags);
    void Map2M(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags);
    void Map1G(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags);
    void Map(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags);
    void Unmap(pagemap_t *pagemap, uint64_t vaddr);
    
    uint64_t GetPhysics(pagemap_t *pagemap, uint64_t vaddr);
    void MapRange(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags, uint64_t count);

    // 内存分配与进程 Fork
    void *Alloc(pagemap_t *pagemap, uint64_t page_count, bool user);
    void Free(pagemap_t *pagemap, void *ptr);
    pagemap_t *Fork(pagemap_t *parent);

    // 缺页中断处理
    uint32_t HandlePF(void *ctx); // context_t 具体类型在 cpp 内部处理

    // VMA 区域管理
    namespace VMA {
        void SetStart(pagemap_t *pagemap, uint64_t start, uint64_t page_count);
        vma_region_t *AddRegion(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags);
        vma_region_t *InsertRegion(vma_region_t *after, uint64_t start, uint64_t page_count, uint64_t flags);
        void RemoveRegion(vma_region_t *region);
    }
    
    // 底层页表查询
    namespace Useless {
        typedef struct {
            uint64_t phys;
            uint64_t size;
            uint64_t flags;
        } PageInfo;

        PageInfo GetPageInfo(pagemap_t *pagemap, uint64_t vaddr);
    }
} // namespace VMM

#endif // _AARCH64_VMM_H