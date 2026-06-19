/*
* SPDX-License-Identifier: GPL-2.0-only
* File: vmm.h
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

#define MM_LARGE_2MB (1ULL << 7)   // x86_64 Page Size (PS) bit
#define PAGE_SIZE_2MB 0x200000ULL  // 2MB in bytes

#define VMM_FLAG_PRESENT        (1 << 0)        /* P   */
#define VMM_FLAG_READWRITE      (1 << 1)        /* R/W */
#define VMM_FLAG_USER           (1 << 2)        /* U/S */
#define VMM_FLAG_WRITETHROUGH   (1 << 3)        /* PWT */
#define VMM_FLAG_CACHE_DISABLE  (1 << 4)        /* PCD */
#define VMM_FLAG_PAT            (1 << 7)        /* PAT */

#define PTE_MMIO (1 << 9)

#define VMM_FLAGS_DEFAULT       (VMM_FLAG_PRESENT | VMM_FLAG_READWRITE)

/* According to Intel's manual, only when CACHE_DISABLE and WRITETHROUGH
 * are both set to 1, the MMIO will be strong uncacheable (UC) which can
 * meet requirements of some memory regions, e.g., xAPIC memory address.
 * If we only set CACHE_DISABLE value, thw writing operation will be halt
 * when running on NEC VersaPro which has a i5-6200U CPU.
 */
#define VMM_FLAGS_MMIO          (VMM_FLAGS_DEFAULT | VMM_FLAG_CACHE_DISABLE \
                                | VMM_FLAG_WRITETHROUGH)
#define VMM_FLAGS_USERMODE      (VMM_FLAGS_DEFAULT | VMM_FLAG_USER)

#define PML5E(x) ((x >> 48) & 0x1ff)
#define PML4E(x) ((x >> 39) & 0x1ff)
#define PDPTE(x) ((x >> 30) & 0x1ff)
#define PDE(x) ((x >> 21) & 0x1ff)
#define PTE(x) ((x >> 12) & 0x1ff)

// 物理地址掩码：只保留 12-51 位 (支持到 52 位物理地址线)
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL

// 标志位掩码：只保留 0-11 位 和 63 位 (NX)
// 屏蔽掉 52-62 位的保留位
#define PTE_FLAGS_MASK 0x8000000000000FFFULL 

#define PTE_MASK(x) (typeof(x))((uint64_t)x & PTE_ADDR_MASK)
#define PTE_FLAGS(x) (typeof(x))((uint64_t)x & PTE_FLAGS_MASK)

#define PAGE_EXISTS(x) ((uint64_t)x & MM_READ)
static inline bool is_user_address(uint64_t addr){
    // 用户态地址 < 0xFFFF800000000000
    return __builtin_expect(addr < 0xFFFF800000000000, 1);
}

static inline bool is_user_buffer_valid(uint64_t addr, size_t count) {
    // 检查是否存在溢出情况
    if (addr > addr + count) return false; 
    
    // 检查起始和结束地址是否都在用户空间内
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
    //volatile uint64_t *pml4;
    volatile uint64_t *toplvl;
    vm_mapping_t *vm_mappings;
    int32_t vma_lock;
    vma_region_t *vma_head;
} pagemap_t;

extern volatile pagemap_t *kernel_pagemap;

namespace VMM{
    
    namespace UserAccess {
        
        /**
         * @brief 将内核数据安全地拷贝到当前用户进程的虚拟地址空间
         * 
         * @param pagemap  当前进程的页表指针
         * @param u_dest   用户态目标虚拟地址
         * @param k_src    内核态源数据地址
         * @param len      拷贝长度
         * @return true    拷贝成功
         * @return false   发生错误（地址非法、未映射或越界）
         */
        void CopyToUser(pagemap_t* pagemap, uint64_t u_dest, const void* k_src, uint64_t len);

        bool CopyFromUser(pagemap_t* pagemap, void* k_dest, const void* u_src, uint64_t len);
    }
    namespace Useless
    {
        uint64_t *NewLevel(uint64_t *level, uint64_t entry);
        uint64_t GetPhysicsFlags(pagemap_t *pagemap, uint64_t vaddr);
        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags);
    } // namespace Useless

    extern "C" void Init();
    
    void Map(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags);
    void Map(uint64_t vaddr, uint64_t paddr);

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
    }

    vm_mapping_t *NewMapping(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags);
    void RemoveMapping(vm_mapping_t *mapping);

    void *Alloc(pagemap_t *pagemap, uint64_t page_count, bool user);
    void Free(pagemap_t *pagemap, void *ptr);

    pagemap_t *Fork(pagemap_t *parent);
    void CleanPM(pagemap_t *pagemap);
    void DestroyPM(pagemap_t *pagemap);

    uint32_t HandlePF(context_t *ctx);


}

#endif