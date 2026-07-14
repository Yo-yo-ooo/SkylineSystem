//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <limine.h>
#include <arch/x86_64/allin.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>

// x86_64 PS (Page Size) Bit
#define VMM_PS_BIT (1ULL << 7)

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

volatile bool IsPM5LVL = 
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

    namespace Useless {

        uint64_t *NewLevel(uint64_t *level, uint64_t entry) {
            uint64_t *new_level = PMM::Request();
            _memset(HIGHER_HALF(new_level), 0, PAGE_SIZE);
            level[entry] = (uint64_t)new_level | 0b111; 
            return new_level;
        }

        PageInfo GetPageInfo(pagemap_t *pagemap, uint64_t vaddr) {
            PageInfo info = {0, 0, 0};
            uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
            if(IsPM5LVL){
                pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
                if(!PAGE_EXISTS(pml4)) return info;
                pml4 = HIGHER_HALF(PTE_MASK(pml4));
            }
#endif
            uint64_t pdpte_val = (uint64_t)pml4[PML4E(vaddr)];
            if (!PAGE_EXISTS(pdpte_val)) return info;
            uint64_t *pdpt = HIGHER_HALF(PTE_MASK(pdpte_val));

            uint64_t pde_val = pdpt[PDPTE(vaddr)];
            if (!PAGE_EXISTS(pde_val)) return info;
            if (pde_val & VMM_PS_BIT) {
                info.phys = pde_val & 0x000FFFFFC0000000ULL;
                info.size = PAGE_1GB;
                info.flags = pde_val & 0x8000000000000FFFULL;
                return info;
            }

            uint64_t *pd = HIGHER_HALF(PTE_MASK(pde_val));
            uint64_t pte_val = pd[PDE(vaddr)];
            if (!PAGE_EXISTS(pte_val)) return info;
            if (pte_val & VMM_PS_BIT) {
                info.phys = pte_val & 0x000FFFFFFFE00000ULL;
                info.size = PAGE_2MB;
                info.flags = pte_val & 0x8000000000000FFFULL;
                return info;
            }

            uint64_t *pt = HIGHER_HALF(PTE_MASK(pte_val));
            uint64_t page_val = pt[PTE(vaddr)];
            if (!PAGE_EXISTS(page_val)) return info;
            info.phys = page_val & 0x000FFFFFFFFFF000ULL;
            info.size = PAGE_SIZE;
            info.flags = page_val & 0x8000000000000FFFULL;
            return info;
        }

        // 转发至 VMA::InternalAlloc
        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags) {
            return VMM::VMA::InternalAlloc(pagemap, page_count, flags);
        }
    } // namespace Useless
    

    void Init(){
        uint64_t pat = 0;
        pat |= (0 << 0);  pat |= (1 << 8);  pat |= (4 << 16); pat |= (6 << 24);
        pat |= (5 << 32); pat |= (1 << 40); pat |= (7 << 48); pat |= (7 << 56);
        wrmsr(0x277, pat);

        struct limine_executable_address_response *executable_address = limine_executable_address.response;
        kernel_pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        kernel_pagemap->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        kernel_pagemap->vma_head = nullptr;
        kernel_pagemap->vma_cursor = nullptr;
        kernel_pagemap->vm_mappings = nullptr;
        _memset(kernel_pagemap->toplvl, 0, PAGE_SIZE);

        // 哨兵节点下界从 HIGHER_HALF(0x100000000000) 开始
        VMM::VMA::SetStart(kernel_pagemap, HIGHER_HALF(0x100000000000), 0);

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

        for (uint64_t gb4 = 0; gb4 < 0x100000000; gb4 += PAGE_2MB) {
            VMM::Map2M(kernel_pagemap, gb4, gb4, MM_READ | MM_WRITE);
            VMM::Map2M(kernel_pagemap, HIGHER_HALF(gb4), gb4, MM_READ | MM_WRITE);
        }

        VMM::SwitchPageMap(kernel_pagemap);
    }

    void Map4K(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags){

        uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if(IsPM5LVL){
            pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
            if (!PAGE_EXISTS(pml4)) pml4 = VMM::Useless::NewLevel(pagemap->toplvl, PML5E(vaddr));
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
#endif
        uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
        if (!PAGE_EXISTS(pdpt)) pdpt = VMM::Useless::NewLevel(pml4, PML4E(vaddr));
        pdpt = HIGHER_HALF(PTE_MASK(pdpt));

        uint64_t *pd = (uint64_t*)pdpt[PDPTE(vaddr)];
        if (!PAGE_EXISTS(pd)) pd = VMM::Useless::NewLevel(pdpt, PDPTE(vaddr));
        pd = HIGHER_HALF(PTE_MASK(pd));

        uint64_t *pt = (uint64_t*)pd[PDE(vaddr)];
        if (!PAGE_EXISTS(pt)) pt = VMM::Useless::NewLevel(pd, PDE(vaddr));
        pt = HIGHER_HALF(PTE_MASK(pt));

        
        pt[PTE(vaddr)] = (paddr & 0x000FFFFFFFFFF000ULL) | (flags & 0x8000000000001FFFULL);

    }

    // --- 2MB 巨页映射 ---
    void Map2M(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags){


        uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if(IsPM5LVL){
            pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
            if (!PAGE_EXISTS(pml4)) pml4 = VMM::Useless::NewLevel(pagemap->toplvl, PML5E(vaddr));
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
#endif
        uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
        if (!PAGE_EXISTS(pdpt)) pdpt = VMM::Useless::NewLevel(pml4, PML4E(vaddr));
        pdpt = HIGHER_HALF(PTE_MASK(pdpt));

        uint64_t *pd = (uint64_t*)pdpt[PDPTE(vaddr)];
        if (!PAGE_EXISTS(pd)) pd = VMM::Useless::NewLevel(pdpt, PDPTE(vaddr));
        pd = HIGHER_HALF(PTE_MASK(pd));

        pd[PDE(vaddr)] = (paddr & 0x000FFFFFFFE00000ULL) | (flags & 0x8000000000001FFFULL) | VMM_PS_BIT;

    }

    // --- 1GB 巨页映射 ---
    void Map1G(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags){
        

        uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if(IsPM5LVL){
            pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
            if (!PAGE_EXISTS(pml4)) pml4 = VMM::Useless::NewLevel(pagemap->toplvl, PML5E(vaddr));
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
#endif
        uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
        if (!PAGE_EXISTS(pdpt)) pdpt = VMM::Useless::NewLevel(pml4, PML4E(vaddr));
        pdpt = HIGHER_HALF(PTE_MASK(pdpt));

        pdpt[PDPTE(vaddr)] = (paddr & 0x000FFFFFC0000000ULL) | (flags & 0x8000000000001FFFULL) | VMM_PS_BIT;

        
    }

    void Map(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags){
        VMM::Map4K(pagemap, vaddr, paddr, flags);
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
        uint64_t pdpte_val = (uint64_t)pml4[PML4E(vaddr)];
        if (!PAGE_EXISTS(pdpte_val)) return;
        uint64_t *pdpt = HIGHER_HALF(PTE_MASK(pdpte_val));

        uint64_t pde_val = pdpt[PDPTE(vaddr)];
        if (!PAGE_EXISTS(pde_val)) return;
        if (pde_val & VMM_PS_BIT) { pdpt[PDPTE(vaddr)] = 0; return; }

        uint64_t *pd = HIGHER_HALF(PTE_MASK(pde_val));
        uint64_t pte_val = pd[PDE(vaddr)];
        if (!PAGE_EXISTS(pte_val)) return;
        if (pte_val & VMM_PS_BIT) { pd[PDE(vaddr)] = 0; return; }

        uint64_t *pt = HIGHER_HALF(PTE_MASK(pte_val));
        pt[PTE(vaddr)] = 0;
    }

    uint64_t GetPhysics(pagemap_t *pagemap, uint64_t vaddr){
        return VMM::Useless::GetPageInfo(pagemap, vaddr).phys;
    }

    void MapRange(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr,
              uint64_t flags, uint64_t count){
        uint64_t mapped = 0;
        while (mapped < count) {
            uint64_t cur_v = vaddr + mapped * PAGE_SIZE;
            uint64_t cur_p = paddr + mapped * PAGE_SIZE;
            uint64_t rem   = count - mapped;

            if ((cur_v & (PAGE_1GB - 1)) == 0 &&
                (cur_p & (PAGE_1GB - 1)) == 0 && rem >= 262144) {
                VMM::Map1G(pagemap, cur_v, cur_p, flags);
                mapped += 262144;
                continue;
            }
            if ((cur_v & (PAGE_2MB - 1)) == 0 &&
                (cur_p & (PAGE_2MB - 1)) == 0 && rem >= 512) {
                VMM::Map2M(pagemap, cur_v, cur_p, flags);
                mapped += 512;
                continue;
            }
            VMM::Map4K(pagemap, cur_v, cur_p, flags);
            mapped += 1;
        }
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
        pagemap->vma_cursor = nullptr;

        /* 新增：初始化红黑树 */
        rb_root_init(&pagemap->vma_tree,
                    nullptr, nullptr,
                    nullptr, nullptr,
                    nullptr);

        __memcpy(&pagemap->toplvl[256],
                &kernel_pagemap->toplvl[256],
                (512 - 256) * sizeof(uint64_t));
        return pagemap;
    }

    
    
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

    // --- Alloc / EAlloc ---
    void *Alloc(pagemap_t *pagemap, uint64_t page_count, bool user) {
        if (!page_count) return nullptr;
        uint64_t flags = MM_READ | MM_WRITE | (user ? MM_USER : 0);

        spinlock_lock(&pagemap->vma_lock);
        uint64_t addr = VMM::VMA::InternalAlloc(pagemap, page_count, flags);
        if (!addr) {
            spinlock_unlock(&pagemap->vma_lock);
            return nullptr;
        }

        uint64_t mapped = 0;
        while (mapped < page_count) {
            uint64_t cv = addr + mapped * PAGE_SIZE;
            uint64_t rem = page_count - mapped;

            if (rem >= 524288 && (cv % PAGE_2GB) == 0) {
                void* phys_ptr = PMM::Request2GB();
                if (phys_ptr) {
                    uint64_t phys = (uint64_t)phys_ptr;
                    VMM::Map1G(pagemap, cv,        phys,             flags);
                    VMM::Map1G(pagemap, cv+PAGE_1GB, phys+PAGE_1GB,  flags);
                    mapped += 524288; continue;
                }
            }
            if (rem >= 512 && (cv % PAGE_2MB) == 0) {
                void* phys_ptr = PMM::Request2MB();
                if (phys_ptr) {
                    VMM::Map2M(pagemap, cv, (uint64_t)phys_ptr, flags);
                    mapped += 512; continue;
                }
            }
            void* phys_ptr = PMM::Request();
            if (!phys_ptr) {
                kerrorln("PMM: OOM in Alloc");
                break; 
            }
            VMM::Map4K(pagemap, cv, (uint64_t)phys_ptr, flags);
            mapped += 1;
        }

        VMM::NewMapping(pagemap, addr, page_count, flags);
        spinlock_unlock(&pagemap->vma_lock);
        return (void*)addr;
    }

    void *EAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags) {
        if (!page_count) return nullptr;

        spinlock_lock(&pagemap->vma_lock);
        uint64_t addr = VMM::VMA::InternalAlloc(pagemap, page_count, flags);
        if (!addr) {
            spinlock_unlock(&pagemap->vma_lock);
            return nullptr;
        }

        uint64_t mapped = 0;
        while (mapped < page_count) {
            uint64_t cv = addr + mapped * PAGE_SIZE;
            uint64_t rem = page_count - mapped;

            if (rem >= 524288 && (cv % PAGE_2GB) == 0) {
                void* phys_ptr = PMM::Request2GB();
                if (phys_ptr) {
                    uint64_t phys = (uint64_t)phys_ptr;
                    VMM::Map1G(pagemap, cv,        phys,             flags);
                    VMM::Map1G(pagemap, cv+PAGE_1GB, phys+PAGE_1GB,  flags);
                    mapped += 524288; continue;
                }
            }
            if (rem >= 512 && (cv % PAGE_2MB) == 0) {
                void* phys_ptr = PMM::Request2MB();
                if (phys_ptr) {
                    VMM::Map2M(pagemap, cv, (uint64_t)phys_ptr, flags);
                    mapped += 512; continue;
                }
            }
            void* phys_ptr = PMM::Request();
            if (!phys_ptr) {
                kerrorln("PMM: OOM in EAlloc");
                break; 
            }
            VMM::Map4K(pagemap, cv, (uint64_t)phys_ptr, flags);
            mapped += 1;
        }

        VMM::NewMapping(pagemap, addr, page_count, flags);
        spinlock_unlock(&pagemap->vma_lock);
        return (void*)addr;
    }

    // --- 感知巨页的 Free ---
    void Free(pagemap_t *pagemap, void *ptr){
        if (((uint64_t)ptr & 0xfff) != 0) return;
        spinlock_lock(&pagemap->vma_lock);

        vma_region_t *region = VMM::VMA::FindRegion(pagemap, (uint64_t)ptr);
        if (!region || region->start != (uint64_t)ptr) {
            spinlock_unlock(&pagemap->vma_lock);
            return;
        }
        pagemap->vma_cursor = region->prev; // 防止游标悬空

        uint64_t v = region->start;
        uint64_t end = v + region->page_count * PAGE_SIZE;
        while (v < end) {
            Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, v);
            if (info.size == 0) { v += PAGE_SIZE; continue; }

            if (info.size == PAGE_1GB) {
                Useless::PageInfo next = VMM::Useless::GetPageInfo(pagemap, v + PAGE_1GB);
                if (next.size == PAGE_1GB && next.phys == info.phys + PAGE_1GB) {
                    PMM::Free2GB((void*)info.phys);
                    VMM::Unmap(pagemap, v + PAGE_1GB);
                    mmu_invlpg(v + PAGE_1GB);
                    v += PAGE_2GB;
                } else {
                    // [BUG 修复] 原代码这里释放512次同一地址！
                    for (uint32_t j = 0; j < 512; j++)
                        PMM::Free2MB((void*)(info.phys + j * PAGE_2MB));
                    v += PAGE_1GB;
                }
            } else if (info.size == PAGE_2MB) {
                PMM::Free2MB((void*)info.phys);
                v += PAGE_2MB;
            } else {
                PMM::Free((void*)info.phys);
                v += PAGE_SIZE;
            }
            VMM::Unmap(pagemap, v - info.size);
            mmu_invlpg(v - info.size);
        }

        // 清理 vm_mapping_t 链表记录
        vm_mapping_t *m = pagemap->vm_mappings;
        if (m) {
            vm_mapping_t *start_m = m;
            do {
                if (m->start == (uint64_t)ptr) {
                    RemoveMapping(m);
                    break;
                }
                m = m->next;
            } while (m != start_m);
        }

        VMM::VMA::RemoveRegion(region);
        spinlock_unlock(&pagemap->vma_lock);
    }

    pagemap_t *Fork(pagemap_t *parent){
        pagemap_t *restore = VMM::SwitchPageMap(kernel_pagemap);
        pagemap_t *pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        pagemap->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        _memset(pagemap->toplvl, 0, PAGE_SIZE);
        for (uint64_t i = 256; i < 512; i++)
            pagemap->toplvl[i] = kernel_pagemap->toplvl[i];

        VMM::VMA::SetStart(pagemap, parent->vma_head->start, 0);

        spinlock_lock(&parent->vma_lock);
        for (vma_region_t *r = parent->vma_head->next; r != parent->vma_head; r = r->next) {
            if (r->start >= HIGHER_HALF(0)) continue;

            uint64_t v = r->start, mapped = 0;
            while (mapped < r->page_count) {
                Useless::PageInfo info = VMM::Useless::GetPageInfo(parent, v);
                if (info.size == 0) break;

                uint64_t nf = (info.flags & ~MM_WRITE) | (1ULL << 55);
                if (info.size == PAGE_1GB) {
                    VMM::Map1G(pagemap, v, info.phys, nf);
                    VMM::Map1G(parent, v, info.phys, nf);
                    mapped += 262144;
                } else if (info.size == PAGE_2MB) {
                    VMM::Map2M(pagemap, v, info.phys, nf);
                    VMM::Map2M(parent, v, info.phys, nf);
                    mapped += 512;
                } else {
                    VMM::Map4K(pagemap, v, info.phys, nf);
                    VMM::Map4K(parent, v, info.phys, nf);
                    mapped += 1;
                }
                v += info.size;
            }
            VMM::VMA::AddRegion(pagemap, r->start, r->page_count, r->flags);
            VMM::NewMapping(pagemap, r->start, r->page_count, r->flags);
        }
        spinlock_unlock(&parent->vma_lock);
        
        VMM::SwitchPageMap(restore);
        return pagemap;
    }

    void CleanPM(pagemap_t *pagemap){
        vma_region_t *r = pagemap->vma_head->next;
        while (r != pagemap->vma_head) {
            vma_region_t *next = r->next;
            if (r->start < HIGHER_HALF(0)) {
                uint64_t v = r->start, end = v + r->page_count * PAGE_SIZE;
                while(v < end) {
                    Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, v);
                    if(info.size) {
                        if(info.size == PAGE_2MB) {
                            PMM::Free2MB((void*)info.phys);
                        } else if(info.size == PAGE_1GB) {
                            Useless::PageInfo n = VMM::Useless::GetPageInfo(pagemap, v+PAGE_1GB);
                            if (n.size == PAGE_1GB && n.phys == info.phys + PAGE_1GB) {
                                PMM::Free2GB((void*)info.phys);
                                VMM::Unmap(pagemap, v + PAGE_1GB);
                                v += PAGE_2GB;
                            } else {
                                for (uint32_t j = 0; j < 512; j++)
                                    PMM::Free2MB((void*)(info.phys + j*PAGE_2MB));
                                v += PAGE_1GB;
                            }
                        } else {
                            PMM::Free((void*)info.phys);
                        }
                        VMM::Unmap(pagemap, v);
                    }
                    v += (info.size ? info.size : PAGE_SIZE);
                }
            }
            VMM::VMA::RemoveRegion(r);
            r = next;
        }
        PMM::Free(PHYSICAL(pagemap->vma_head));
        pagemap->vma_head = pagemap->vma_cursor = nullptr;

        vm_mapping_t *mapping = pagemap->vm_mappings;
        if(mapping) {
            vm_mapping_t *start_mapping = mapping;
            bool cont = true;
            do {
                vm_mapping_t *next = mapping->next;
                if (next == start_mapping) cont = false;
                VMM::RemoveMapping(mapping);
                mapping = next;
            } while (cont);
        }
        pagemap->vm_mappings = nullptr;
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
        if (!smp_started || !this_cpu() || !t) {
            kerror("Page fault on 0x%p, Should NOT continue.\n", cr2);
            return 1; 
        }
        
        pagemap_t *restore = VMM::SwitchPageMap(kernel_pagemap);
        uint64_t fault_addr = ALIGN_DOWN(cr2, PAGE_SIZE);
        pagemap_t *pagemap = t->pagemap;
        
        Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, fault_addr);
        uint64_t old_phys = info.phys;
        if (!old_phys) {
            kerrorln("Segmentation fault (core undumped)");
            VMM::SwitchPageMap(restore);
            return 1;
        }

        bool is_cow = (info.flags & (1ULL << 55));
        if (!wr || !is_cow) {
            kerrorln("Illegal #PF (not CoW or not write)");
            VMM::SwitchPageMap(restore);
            return 1;
        }

        uint64_t new_flags = info.flags & ~(1ULL << 55);
        new_flags |= MM_WRITE;

        if (info.size == PAGE_1GB) {
            uint64_t page_start = fault_addr & ~(PAGE_1GB - 1);
            VMM::Unmap(pagemap, page_start); 
            __asm__ volatile ("invlpg (%0)" : : "r"(page_start) : "memory");
            
            for (int j = 0; j < 512; j++) {
                uint64_t v = page_start + j * PAGE_2MB;
                uint64_t p = old_phys + j * PAGE_2MB;
                uint64_t new_phys = (uint64_t)PMM::Request2MB();
                if (!new_phys) {
                    kerrorln("OOM during 1G CoW split");
                    VMM::SwitchPageMap(restore);
                    return 1;
                }
                __memcpy(HIGHER_HALF((void*)new_phys), HIGHER_HALF((void*)p), PAGE_2MB);
                VMM::Map2M(pagemap, v, new_phys, new_flags);
            }
        } else if (info.size == PAGE_2MB) {
            uint64_t page_start = fault_addr & ~(PAGE_2MB - 1);
            uint64_t new_phys = (uint64_t)PMM::Request2MB();
            __memcpy(HIGHER_HALF((void*)new_phys), HIGHER_HALF((void*)old_phys), PAGE_2MB);
            VMM::Map2M(pagemap, page_start, new_phys, new_flags);
        } else {
            uint64_t new_phys = (uint64_t)PMM::Request();
            __memcpy(HIGHER_HALF((void*)new_phys), HIGHER_HALF((void*)old_phys), PAGE_SIZE);
            VMM::Map4K(pagemap, fault_addr, new_phys, new_flags);
        }
        
        __asm__ volatile ("invlpg (%0)" : : "r"(fault_addr) : "memory");
        VMM::SwitchPageMap(restore);
        return 0;
    }

}