/*
* SPDX-License-Identifier: GPL-2.0-only
* File: vmm.cpp
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
#include <limine.h>
#include <arch/x86_64/allin.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request limine_executable_address = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0
};

#if CONFIG_VMM_5LVL_MAP == 1
#define REQ_TOP_LVL LIMINE_PAGING_MODE_X86_64_5LVL
#else
#define REQ_TOP_LVL LIMINE_PAGING_MODE_X86_64_4LVL
#endif

__attribute__((used, section(".limine_requests")))
static volatile struct limine_paging_mode_request paging_mode_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID,
    .revision = 0,
    .mode = REQ_TOP_LVL,
    .max_mode = REQ_TOP_LVL,
    .min_mode = LIMINE_PAGING_MODE_X86_64_MIN
};

static volatile bool IsPM5LVL = 
    (paging_mode_request.response->mode == REQ_TOP_LVL);

extern volatile spinlock_t pmm_lock;
#define PHYS_BASE(x) (x - executable_vaddr + executable_paddr)

volatile pagemap_t* kernel_pagemap = nullptr;

extern "C" void mmu_invlpg(uint64_t vaddr);

extern struct limine_memmap_response* pmm_memmap;

extern char ld_limine_start[];
extern char ld_limine_end[];
extern char ld_text_start[];
extern char ld_text_end[];
extern char ld_rodata_start[];
extern char ld_rodata_end[];
extern char ld_data_start[];
extern char ld_data_end[];

extern bool smp_started;

namespace VMM {

    namespace Useless
    {
        uint64_t InternalFree(pagemap_t *pagemap, uint64_t addr, uint64_t page_count) {
            if (!pagemap->vma_head) return -1;
            vma_region_t *region = pagemap->vma_head->next;
            for (; region != pagemap->vma_head; region = region->next) {
                if (region->start == addr && region->page_count == page_count) {
                    VMM::VMA::RemoveRegion(region);
                    return 0;
                }
            }
            return -1;
        }

        uint64_t *NewLevel(uint64_t *level, uint64_t entry) {
            uint64_t *new_level = (uint64_t*)PMM::Request();
            _memset(HIGHER_HALF(new_level), 0, PAGE_SIZE);
            level[entry] = (uint64_t)new_level | 0b111;
            return new_level;
        }

        uint64_t GetPhysicsFlags(pagemap_t *pagemap, uint64_t vaddr) {
            uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
            if(IsPM5LVL){
                pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
                if(!PAGE_EXISTS(pml4)) return 0;
                pml4 = HIGHER_HALF(PTE_MASK(pml4));
            }
#endif
            uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
            if (!PAGE_EXISTS(pdpt)) return 0;
            pdpt = HIGHER_HALF(PTE_MASK(pdpt));

            uint64_t *pd = (uint64_t*)pdpt[PDPTE(vaddr)];
            if (!PAGE_EXISTS(pd)) return 0;
            pd = HIGHER_HALF(PTE_MASK(pd));

            uint64_t pde_val = pd[PDE(vaddr)];
            if (pde_val & MM_LARGE_2MB) {
                return pde_val;
            }

            uint64_t *pt = (uint64_t*)pde_val;
            if (!PAGE_EXISTS(pt)) return 0;
            pt = HIGHER_HALF(PTE_MASK(pt));

            return pt[PTE(vaddr)];
        }

        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags) {
            if (!pagemap->vma_head) return 0;
            uint64_t align_val = (page_count >= 512) ? PAGE_SIZE_2MB : PAGE_SIZE;
            uint64_t alloc_size = page_count * PAGE_SIZE; 
            
            vma_region_t *region = pagemap->vma_head->next;
            uint64_t addr = 0;
            
            if (region == pagemap->vma_head) {
                addr = ALIGN_UP(region->start + (region->page_count * PAGE_SIZE), align_val);
                VMM::VMA::AddRegion(pagemap, addr, page_count, flags);
                return addr;
            }
            
            for (; region != pagemap->vma_head; region = region->next) {
                if (region->next == pagemap->vma_head) {
                    addr = ALIGN_UP(region->start + (region->page_count * PAGE_SIZE), align_val);
                    VMM::VMA::AddRegion(pagemap, addr, page_count, flags);
                    return addr;
                }
                uint64_t region_end = region->start + (region->page_count * PAGE_SIZE);
                addr = ALIGN_UP(region_end, align_val);
                
                if (region->next->start >= addr + alloc_size) {
                    region = VMM::VMA::InsertRegion(region, addr, page_count, flags);
                    return addr;
                }
            }
            return 0;
        }
    } // namespace Useless

    void Init(){
        uint64_t pat = 0;
        pat |= (0ULL << 0);
        pat |= (1ULL << 8);
        pat |= (4ULL << 16);
        pat |= (6ULL << 24);
        pat |= (5ULL << 32);
        pat |= (1ULL << 40);
        pat |= (7ULL << 48);
        pat |= (7ULL << 56);

        wrmsr(0x277, pat);

        struct limine_executable_address_response *executable_address = limine_executable_address.response;
        kernel_pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        kernel_pagemap->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        kernel_pagemap->vma_head = nullptr;
        _memset(kernel_pagemap->toplvl, 0, PAGE_SIZE);
#if CONFIG_VMM_5LVL_MAP == 1
        if(IsPM5LVL)
            kinfoln("THIS CPU SUPPORT 5 LVL PAGING!");
#endif
        VMM::VMA::SetStart(kernel_pagemap, HIGHER_HALF(0x100000000000), 1);

        uint64_t executable_vaddr = executable_address->virtual_base;
        uint64_t executable_paddr = executable_address->physical_base;

        uint64_t limine_start = ALIGN_DOWN((uint64_t)ld_limine_start, PAGE_SIZE);
        uint64_t limine_end   = ALIGN_UP((uint64_t)ld_limine_end, PAGE_SIZE);
        uint64_t limine_pages = DIV_ROUND_UP(limine_end - limine_start, PAGE_SIZE);

        uint64_t text_start = ALIGN_DOWN((uint64_t)ld_text_start, PAGE_SIZE);
        uint64_t text_end   = ALIGN_UP((uint64_t)ld_text_end, PAGE_SIZE);
        uint64_t text_pages = DIV_ROUND_UP(text_end - text_start, PAGE_SIZE);

        uint64_t rodata_start = ALIGN_DOWN((uint64_t)ld_rodata_start, PAGE_SIZE);
        uint64_t rodata_end   = ALIGN_UP((uint64_t)ld_rodata_end, PAGE_SIZE);
        uint64_t rodata_pages = DIV_ROUND_UP(rodata_end - rodata_start, PAGE_SIZE);

        uint64_t data_start = ALIGN_DOWN((uint64_t)ld_data_start, PAGE_SIZE);
        uint64_t data_end   = ALIGN_UP((uint64_t)ld_data_end, PAGE_SIZE);
        uint64_t data_pages = DIV_ROUND_UP(data_end - data_start, PAGE_SIZE);

        VMM::MapRange(kernel_pagemap, limine_start, PHYS_BASE(limine_start), MM_READ, limine_pages);
        VMM::MapRange(kernel_pagemap, text_start, PHYS_BASE(text_start), MM_READ, text_pages);
        VMM::MapRange(kernel_pagemap, rodata_start, PHYS_BASE(rodata_start), MM_READ | MM_NX, rodata_pages);
        VMM::MapRange(kernel_pagemap, data_start, PHYS_BASE(data_start), MM_READ | MM_WRITE | MM_NX, data_pages);

        for (uint64_t gb4 = 0; gb4 < 0x100000000; gb4 += PAGE_SIZE) {
            VMM::Map(kernel_pagemap, gb4, gb4, MM_READ | MM_WRITE);
            VMM::Map(kernel_pagemap, HIGHER_HALF(gb4), gb4, MM_READ | MM_WRITE);
        }

        VMM::SwitchPageMap(kernel_pagemap);
    }

    void Map(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
        uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
    #if CONFIG_VMM_5LVL_MAP == 1
        if(IsPM5LVL){
            pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
            if (!PAGE_EXISTS(pml4))
                pml4 = VMM::Useless::NewLevel(pagemap->toplvl, PML5E(vaddr));
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
    #endif

        uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
        if (!PAGE_EXISTS(pdpt))
            pdpt = VMM::Useless::NewLevel(pml4, PML4E(vaddr));
        pdpt = HIGHER_HALF(PTE_MASK(pdpt));

        uint64_t *pd = (uint64_t*)pdpt[PDPTE(vaddr)];
        if (!PAGE_EXISTS(pd))
            pd = VMM::Useless::NewLevel(pdpt, PDPTE(vaddr));
        pd = HIGHER_HALF(PTE_MASK(pd));

        if (flags & MM_LARGE_2MB) {
            pd[PDE(vaddr)] = (paddr & 0x000FFFFFFFE00000ULL) | (flags & 0x8000000000000FFFULL);
            return;
        }

        uint64_t *pt = (uint64_t*)pd[PDE(vaddr)];
        if (!PAGE_EXISTS(pt))
            pt = VMM::Useless::NewLevel(pd, PDE(vaddr));
        pt = HIGHER_HALF(PTE_MASK(pt));
        pt[PTE(vaddr)] = (paddr & 0x000FFFFFFFFFF000ULL) | (flags & 0x8000000000000FFFULL);
    }

    void Map(uint64_t vaddr, uint64_t paddr){
        VMM::Map(kernel_pagemap, vaddr, paddr, MM_READ | MM_WRITE);
    }

    void Unmap(pagemap_t *pagemap, uint64_t vaddr){
        uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if(IsPM5LVL){
            pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
            if (!PAGE_EXISTS(pml4)) return;
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
#endif
        uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
        if (!PAGE_EXISTS(pdpt)) return;
        pdpt = HIGHER_HALF(PTE_MASK(pdpt));

        uint64_t *pd = (uint64_t*)pdpt[PDPTE(vaddr)];
        if (!PAGE_EXISTS(pd)) return;
        pd = HIGHER_HALF(PTE_MASK(pd));

        if (pd[PDE(vaddr)] & MM_LARGE_2MB) {
            pd[PDE(vaddr)] = 0; 
            return;
        }

        uint64_t *pt = (uint64_t*)pd[PDE(vaddr)];
        if (!PAGE_EXISTS(pt)) return;
        pt = HIGHER_HALF(PTE_MASK(pt));

        pt[PTE(vaddr)] = 0;
    }

    uint64_t GetPhysics(pagemap_t *pagemap, uint64_t vaddr){
        uint64_t page = VMM::Useless::GetPhysicsFlags(pagemap, vaddr);
        if (!page) return 0;
        return PTE_MASK(page);
    }

    void MapRange(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags, uint64_t count){
        for (uint64_t i = 0; i < count; i++)
            VMM::Map(pagemap, vaddr + (i * PAGE_SIZE), paddr + (i * PAGE_SIZE), flags);
    }

    pagemap_t *SwitchPageMap(pagemap_t *pagemap){
        asm volatile("cli");
        pagemap_t *old_pagemap = nullptr;
        if (smp_started) {
            old_pagemap = this_cpu()->pagemap;
            this_cpu()->pagemap = pagemap;
        }
        __asm__ volatile ("movq %0, %%cr3" : : "r"(PHYSICAL((uint64_t)pagemap->toplvl)) : "memory");
        asm volatile("sti");
        return old_pagemap;
    }

    pagemap_t *NewPM(){
        pagemap_t *pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        pagemap->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        _memset(pagemap->toplvl, 0, PAGE_SIZE);
        pagemap->vm_mappings = nullptr;
        pagemap->vma_lock = 0;
        pagemap->vma_head = nullptr;
        __memcpy(&pagemap->toplvl[256], 
            &kernel_pagemap->toplvl[256], 
            (512 - 256) * sizeof(uint64_t));
        return pagemap;
    }

    namespace VMA
    {
        void SetStart(pagemap_t *pagemap, uint64_t start, uint64_t page_count){
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
            region->start = start;
            region->page_count = page_count;
            region->flags = MM_READ | MM_WRITE;
            region->next = region;
            region->prev = region;
            pagemap->vma_head = region;
        }
        vma_region_t *AddRegion(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags){
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
            region->start = start;
            region->page_count = page_count;
            region->flags = flags;
            region->prev = pagemap->vma_head->prev;
            region->next = pagemap->vma_head;
            pagemap->vma_head->prev->next = region;
            pagemap->vma_head->prev = region;
            return region;
        }
        vma_region_t *InsertRegion(vma_region_t *after, uint64_t start, uint64_t page_count, uint64_t flags){
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
            region->start = start;
            region->page_count = page_count;
            region->flags = flags;
            region->prev = after;
            region->next = after->next;
            after->next->prev = region;
            after->next = region;
            return region;
        }

        void RemoveRegion(vma_region_t *region) {
            region->next->prev = region->prev;
            region->prev->next = region->next;
            PMM::Free(PHYSICAL((void*)region));
        }
    } // namespace VMA

    vm_mapping_t *NewMapping(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags){
        vm_mapping_t *mapping = HIGHER_HALF((vm_mapping_t*)PMM::Request());
        mapping->start = start;
        mapping->page_count = page_count;
        mapping->flags = flags;
        if (pagemap->vm_mappings) {
            mapping->prev = pagemap->vm_mappings->prev;
            mapping->next = pagemap->vm_mappings;
            pagemap->vm_mappings->prev->next = mapping;
            pagemap->vm_mappings->prev = mapping;
            return mapping;
        }
        mapping->prev = mapping->next = mapping;
        pagemap->vm_mappings = mapping;
        return mapping;
    }
    
    void RemoveMapping(vm_mapping_t *mapping){
        mapping->next->prev = mapping->prev;
        mapping->prev->next = mapping->next;
        PMM::Free(PHYSICAL((void*)mapping));
    }

    void *Alloc(pagemap_t *pagemap, uint64_t page_count, bool user) {
        if (__builtin_expect(!page_count, 0)) return nullptr;

        uint64_t flags = MM_READ | MM_WRITE | (user ? MM_USER : 0);

        spinlock_lock(&pagemap->vma_lock);
        uint64_t addr = VMM::Useless::InternalAlloc(pagemap, page_count, flags);
        if (__builtin_expect(!addr, 0)) {
            spinlock_unlock(&pagemap->vma_lock);
            return nullptr;
        }

        Interrupt::Mask();
        cpu_t* cpu = this_cpu();
        uint64_t i = 0;

        while ((page_count - i) >= 512) {
            spinlock_lock(&pmm_lock);
            void* phys_ptr = PMM::RequestHuge();
            spinlock_unlock(&pmm_lock);

            if (__builtin_expect(!!phys_ptr, 1)) {
                VMM::Map(pagemap, addr + (i * PAGE_SIZE), (uint64_t)phys_ptr, flags | MM_LARGE_2MB);
                i += 512;
            } else {
                break;
            }
        }

        for (; i < page_count; i++) {
            void* phys_ptr = nullptr;

            if (__builtin_expect(!!(cpu && cpu->pmm_cache_count > 0), 1)) {
                phys_ptr = cpu->pmm_cache[--cpu->pmm_cache_count];
            } else {
                spinlock_lock(&pmm_lock);
                phys_ptr = PMM::GlobalRequestSingle();
                
                if (__builtin_expect(!!(phys_ptr && cpu), 1)) {
                    while (cpu->pmm_cache_count < PMM_PCP_BATCH) {
                        void* batch_page = PMM::GlobalRequestSingle();
                        if (!batch_page) break;
                        cpu->pmm_cache[cpu->pmm_cache_count++] = batch_page;
                    }
                }
                spinlock_unlock(&pmm_lock);
            }

            if (__builtin_expect(!phys_ptr, 0)) {
                kerrorln("PMM: Out of memory at page %d (VA: 0x%lx)!", i, addr);
                goto oom_rollback;
            }
            
            VMM::Map(pagemap, addr + (i * PAGE_SIZE), (uint64_t)phys_ptr, flags);
        }
        
        Interrupt::Unmask(); 
        
        VMM::NewMapping(pagemap, addr, page_count, flags);
        spinlock_unlock(&pagemap->vma_lock);

        return (void*)addr; 

    oom_rollback:
        Interrupt::Unmask();
        
        uint64_t j = 0;
        while (j < i) {
            uint64_t va = addr + j * PAGE_SIZE;
            uint64_t pt_flags = VMM::Useless::GetPhysicsFlags(pagemap, va);
            uint64_t pa = PTE_MASK(pt_flags);
            
            if (pt_flags & MM_LARGE_2MB) {
                if (pa) PMM::FreeHuge((void*)pa);
                VMM::Unmap(pagemap, va);
                j += 512;
            } else {
                if (pa) PMM::Free((void*)pa);
                VMM::Unmap(pagemap, va);
                j++;
            }
            mmu_invlpg(va);
        }
        
        VMM::Useless::InternalFree(pagemap, addr, page_count);
        spinlock_unlock(&pagemap->vma_lock);
        return nullptr;
    }

    void Free(pagemap_t *pagemap, void *ptr){
        if (((uint64_t)ptr & 0xfff) != 0) return;
        
        spinlock_lock(&pagemap->vma_lock);
        if (!pagemap->vma_head) {
            spinlock_unlock(&pagemap->vma_lock);
            return;
        }

        vma_region_t *region = pagemap->vma_head->next;
        for (; region != pagemap->vma_head; region = region->next) {
            if (region->start == (uint64_t)ptr) {
                uint64_t i = 0;
                while (i < region->page_count) {
                    uint64_t virt_addr = region->start + (i * PAGE_SIZE);
                    uint64_t pt_flags = VMM::Useless::GetPhysicsFlags(pagemap, virt_addr);
                    uint64_t phys_addr = PTE_MASK(pt_flags);
                    
                    if (pt_flags & MM_LARGE_2MB) {
                        if (phys_addr) PMM::FreeHuge((void*)phys_addr);
                        VMM::Unmap(pagemap, virt_addr);
                        i += 512;
                    } else {
                        if (phys_addr) PMM::Free((void*)phys_addr);
                        VMM::Unmap(pagemap, virt_addr);
                        i++;
                    }
                    mmu_invlpg(virt_addr);
                }
                VMM::VMA::RemoveRegion(region);
                spinlock_unlock(&pagemap->vma_lock);
                return;
            }
        }
        spinlock_unlock(&pagemap->vma_lock);
    }

    pagemap_t *Fork(pagemap_t *parent){
        pagemap_t *restore = VMM::SwitchPageMap(kernel_pagemap);
        pagemap_t *pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        pagemap->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        _memset(pagemap->toplvl, 0, PAGE_SIZE);
        
        for (uint64_t i = 256; i < 512; i++)
            pagemap->toplvl[i] = kernel_pagemap->toplvl[i];
        
        if (parent->vm_mappings) {
            vm_mapping_t *mapping = parent->vm_mappings;
            while (true) {
                uint64_t page_flags = mapping->flags;
                uint64_t new_flags = page_flags & ~MM_WRITE;
                new_flags |= (1ULL << 55); 
                
                uint64_t i = 0;
                while (i < mapping->page_count) {
                    uint64_t virt_addr = mapping->start + (i * PAGE_SIZE);
                    uint64_t pt_flags = VMM::Useless::GetPhysicsFlags(parent, virt_addr);
                    uint64_t phys_addr = PTE_MASK(pt_flags);

                    if (pt_flags & MM_LARGE_2MB) {
                        VMM::Map(pagemap, virt_addr, phys_addr, new_flags | MM_LARGE_2MB);
                        VMM::Map(parent, virt_addr, phys_addr, new_flags | MM_LARGE_2MB);
                        i += 512;
                    } else {
                        VMM::Map(pagemap, virt_addr, phys_addr, new_flags);
                        VMM::Map(parent, virt_addr, phys_addr, new_flags);
                        i++;
                    }
                }
                VMM::NewMapping(pagemap, mapping->start, mapping->page_count, page_flags);
                mapping = mapping->next;
                if (mapping == parent->vm_mappings)
                    break;
            }
        }

        if (parent->vma_head) {
            VMM::VMA::SetStart(pagemap, parent->vma_head->start, parent->vma_head->page_count);
            spinlock_lock(&parent->vma_lock);
            vma_region_t *region = parent->vma_head->next;
            for (; region != parent->vma_head; region = region->next)
                VMM::VMA::AddRegion(pagemap, region->start, region->page_count, region->flags);
            spinlock_unlock(&parent->vma_lock);
        }

        VMM::SwitchPageMap(restore);
        return pagemap;
    }

    void CleanPM(pagemap_t *pagemap){
        if (pagemap->vma_head) {
            vma_region_t *region = pagemap->vma_head->next;
            vma_region_t *next_region;
            for (; region != pagemap->vma_head; region = next_region) {
                next_region = region->next;
                uint64_t i = 0;
                while (i < region->page_count) {
                    uint64_t virt_addr = region->start + (i * PAGE_SIZE);
                    uint64_t pt_flags = VMM::Useless::GetPhysicsFlags(pagemap, virt_addr);
                    uint64_t phys_addr = PTE_MASK(pt_flags);

                    if (pt_flags & MM_LARGE_2MB) {
                        if (phys_addr) PMM::FreeHuge((void*)phys_addr);
                        VMM::Unmap(pagemap, virt_addr);
                        i += 512;
                    } else {
                        if (phys_addr) PMM::Free((void*)phys_addr);
                        VMM::Unmap(pagemap, virt_addr);
                        i++;
                    }
                }
                VMM::VMA::RemoveRegion(region);
            }
            PMM::Free(PHYSICAL(pagemap->vma_head));
            pagemap->vma_head = nullptr;
        }

        if (pagemap->vm_mappings) {
            vm_mapping_t *mapping = pagemap->vm_mappings->next;
            vm_mapping_t *start_mapping = pagemap->vm_mappings;
            bool cont = true;
            do {
                vm_mapping_t *next = mapping->next;
                if (next == start_mapping)
                    cont = false;
                
                uint64_t i = 0;
                while (i < mapping->page_count) {
                    uint64_t virt_addr = mapping->start + (i * PAGE_SIZE);
                    uint64_t pt_flags = VMM::Useless::GetPhysicsFlags(pagemap, virt_addr);
                    if (pt_flags & MM_LARGE_2MB) {
                        VMM::Unmap(pagemap, virt_addr);
                        i += 512;
                    } else {
                        VMM::Unmap(pagemap, virt_addr);
                        i++;
                    }
                }
                VMM::RemoveMapping(mapping);
                mapping = next;
            } while (cont);
            pagemap->vm_mappings = nullptr;
        }
    }

    void DestroyPM(pagemap_t *pagemap){
        VMM::CleanPM(pagemap);
        PMM::Free(PHYSICAL(pagemap->toplvl));
        PMM::Free(PHYSICAL(pagemap));
    }

    uint32_t HandlePF(context_t *ctx){
        uint64_t cr2 = 0;
        __asm__ volatile ("movq %%cr2, %0" : "=r"(cr2));

        bool p  = ctx->error_code & (1 << 0); 
        bool wr = ctx->error_code & (1 << 1); 
        bool us = ctx->error_code & (1 << 2); 

        kinfoln("[#PF] Addr: 0x%p | RIP: 0x%p | Cause: %s %s in %s mode",
                cr2, ctx->rip, 
                p ? "Protection-Violation" : "Non-Present",
                wr ? "(Write)" : "(Read)",
                us ? "User" : "Kernel");

        thread_t *t = Schedule::this_thread();
        kinfoln("Handle PF Addr %llu", cr2);

        if (!smp_started || !this_cpu() || !t) {
            kerror("Page fault on 0x%p, Should NOT continue.\n", cr2);
            return 1; 
        }

        pagemap_t *restore = VMM::SwitchPageMap(kernel_pagemap);
        uint64_t fault_addr = ALIGN_DOWN(cr2, PAGE_SIZE);
        pagemap_t *pagemap = t->pagemap;
        
        uint64_t page = VMM::Useless::GetPhysicsFlags(pagemap, fault_addr);
        uint64_t old_phys = PTE_MASK(page);
        kinfoln("Page %llu", page);

        if (!old_phys) {
            kerror("Page fault on thread (0x%p).\n", cr2);
            kerrorln("Segmentation fault (core undumped)");
            VMM::SwitchPageMap(restore);
            return 1;
        }

        uint64_t new_flags = PTE_FLAGS(page);
        new_flags &= ~(1ULL << 55);
        new_flags |= MM_WRITE;
        
        uint64_t new_phys = (uint64_t)PMM::Request();
        __memcpy(HIGHER_HALF((void*)new_phys), HIGHER_HALF((void*)old_phys), PAGE_SIZE);
        
        VMM::Map(pagemap, fault_addr, new_phys, new_flags);
        
        VMM::SwitchPageMap(restore);
        __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr));
        
        return 0;
    }
}