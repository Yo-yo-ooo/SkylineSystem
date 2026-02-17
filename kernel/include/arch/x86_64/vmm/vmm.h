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

#define VMM_FLAG_PRESENT        (1 << 0)        /* P   */
#define VMM_FLAG_READWRITE      (1 << 1)        /* R/W */
#define VMM_FLAG_USER           (1 << 2)        /* U/S */
#define VMM_FLAG_WRITETHROUGH   (1 << 3)        /* PWT */
#define VMM_FLAG_CACHE_DISABLE  (1 << 4)        /* PCD */
#define VMM_FLAG_PAT            (1 << 7)        /* PAT */

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

#define PML4E(x) ((x >> 39) & 0x1ff)
#define PDPTE(x) ((x >> 30) & 0x1ff)
#define PDE(x) ((x >> 21) & 0x1ff)
#define PTE(x) ((x >> 12) & 0x1ff)

#define PTE_MASK(x) (typeof(x))((uint64_t)x & 0x000ffffffffff000)
#define PTE_FLAGS(x) (typeof(x))((uint64_t)x & 0xfff0000000000fff)

#define PAGE_EXISTS(x) ((uint64_t)x & MM_READ)

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
    volatile uint64_t *pml4;
    vm_mapping_t *vm_mappings;
    int32_t vma_lock;
    vma_region_t *vma_head;
} pagemap_t;

extern volatile pagemap_t *kernel_pagemap;

namespace VMM{
    

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

    uint8_t HandlePF(context_t *ctx);

}

#endif