// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <limine.h>
#include <arch/x86_64/smp/smp.h>
#include <conf.h>
#include <arch/x86_64/vmm/vmm.h>
#include <klib/klib.h>
#include <arch/x86_64/lapic/lapic.h>
#include <mem/pmm.h>
#include <klib/kio.h>
#include <arch/x86_64/schedule/sched.h>

#define VMM_PS_BIT  (1ULL << 7)
#define VMM_COW_BIT (1ULL << 55)              // ★ CoW 软件位

#define PTE_KEEP    0xFFF0000000000FFFULL


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

volatile bool IsPM5LVL = (paging_mode_request.response->mode == REQ_TOP_LVL);

extern spinlock_t pmm_lock;
#define PHYS_BASE(x) (x - executable_vaddr + executable_paddr)

volatile pagemap_t* kernel_pagemap = nullptr;

extern "C" void mmu_invlpg(uint64_t vaddr);
extern struct limine_memmap_response* pmm_memmap;

extern char ld_limine_start[], ld_limine_end[], ld_text_start[], ld_text_end[];
extern char ld_rodata_start[], ld_rodata_end[], ld_data_start[], ld_data_end[];

extern bool smp_started;

#ifndef MAX_CPU
#define MAX_CPU 256
#endif

#define TLB_MASK_WORDS ((MAX_CPU + 63) / 64)

namespace VMM {
    namespace CPUFeatures {
        bool has_pcid = false;
        bool has_invpcid = false;
    }

    static spinlock_t pcid_alloc_lock = 0;
    static uint64_t pcid_bitmap[64] = {0};

    static inline uint32_t AllocPCID() {
        if (!CPUFeatures::has_pcid || !CPUFeatures::has_invpcid) return 0;
        spinlock_lock(&pcid_alloc_lock);
        pcid_bitmap[0] |= 1; // Reserve PCID 0
        for (int i = 0; i < 64; i++) {
            if (pcid_bitmap[i] != ~0ULL) {
                int bit = __builtin_ffsll(~pcid_bitmap[i]) - 1;
                uint32_t pcid = i * 64 + bit;
                pcid_bitmap[i] |= (1ULL << bit);
                spinlock_unlock(&pcid_alloc_lock);
                return pcid;
            }
        }
        spinlock_unlock(&pcid_alloc_lock);
        return 0;
    }

    static inline void FreePCID(uint32_t pcid) {
        if (pcid == 0) return;
        spinlock_lock(&pcid_alloc_lock);
        pcid_bitmap[pcid / 64] &= ~(1ULL << (pcid % 64));
        spinlock_unlock(&pcid_alloc_lock);
    }

    static inline void BitmapSet(uint64_t *map, uint32_t cpu) {
        __atomic_fetch_or(&map[cpu / 64], 1ULL << (cpu % 64), __ATOMIC_RELAXED);
    }
    static inline void BitmapClear(uint64_t *map, uint32_t cpu) {
        __atomic_fetch_and(&map[cpu / 64], ~(1ULL << (cpu % 64)), __ATOMIC_RELAXED);
    }
    static inline void BitmapClearAll(uint64_t *map) {
        for (int i = 0; i < TLB_MASK_WORDS; i++)
            __atomic_store_n(&map[i], 0, __ATOMIC_RELAXED);
    }

    #define TLB_FLUSH_FULL   ((uint64_t)-1)
    #define TLB_FLUSH_VEC    (SCHED_VEC + 1)

    namespace LazyTLB {
        static inline void cpuid(uint32_t leaf, uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d) {
            __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(leaf));
        }

        void IPIHandler(context_t *ctx);

        void Init() {
            uint32_t a, b, c, d;
            cpuid(1, a, b, c, d);
            CPUFeatures::has_pcid = (c >> 17) & 1;

            cpuid(7, a, b, c, d);
            CPUFeatures::has_invpcid = (b >> 10) & 1;

            if (CPUFeatures::has_pcid && CPUFeatures::has_invpcid) {
                uint64_t cr4;
                __asm__ volatile ("movq %%cr4, %0" : "=r"(cr4));
                cr4 |= (1ULL << 17); // CR4.PCIDE
                __asm__ volatile ("movq %0, %%cr4" :: "r"(cr4));
            } else {
                CPUFeatures::has_pcid = false;
            }

            idt_install_irq(TLB_FLUSH_VEC, (void*)IPIHandler);
            idt_set_ist(TLB_FLUSH_VEC, 0);
        }

        static inline void LocalInvlpg(pagemap_t *pm, uint64_t vaddr) {
            if (CPUFeatures::has_invpcid) {
                struct { uint64_t vaddr; uint64_t pcid; } __attribute__((aligned(16))) desc = { vaddr, pm ? pm->pcid : 0 };
                uint64_t type = 0;
                __asm__ volatile ("invpcid %1, %0" : : "r"(type), "m"(desc) : "memory");
            } else {
                __asm__ volatile ("invlpg (%0)" :: "r"(vaddr) : "memory");
            }
        }

        static inline void LocalFullFlush(pagemap_t *pm) {
            if (CPUFeatures::has_invpcid) {
                struct { uint64_t vaddr; uint64_t pcid; } __attribute__((aligned(16))) desc = { 0, pm ? pm->pcid : 0 };
                uint64_t type = 1;
                __asm__ volatile ("invpcid %1, %0" : : "r"(type), "m"(desc) : "memory");
            } else {
                uint64_t cr3;
                __asm__ volatile ("movq %%cr3, %0\n\tmovq %0, %%cr3" : "=&r"(cr3) :: "memory");
            }
        }

        static inline void LocalGlobalFlush() {
            if (CPUFeatures::has_invpcid) {
                struct { uint64_t vaddr; uint64_t pcid; } __attribute__((aligned(16))) desc = { 0, 0 };
                uint64_t type = 3;
                __asm__ volatile ("invpcid %1, %0" : : "r"(type), "m"(desc) : "memory");
            } else {
                uint64_t cr4;
                __asm__ volatile ("movq %%cr4, %0" : "=r"(cr4));
                cr4 &= ~(1ULL << 7);
                __asm__ volatile ("movq %0, %%cr4" :: "r"(cr4) : "memory");
                uint64_t cr3;
                __asm__ volatile ("movq %%cr3, %0\n\tmovq %0, %%cr3" : "=&r"(cr3) :: "memory");
                cr4 |= (1ULL << 7);
                __asm__ volatile ("movq %0, %%cr4" :: "r"(cr4) : "memory");
            }
        }

        void OnAttach(pagemap_t *pm) {
            if (!smp_started || !pm) return;
            BitmapSet(pm->cpus_with_tlb, this_cpu()->id);
        }

        void OnDetach(pagemap_t *pm) {
            if (!smp_started || !pm || CPUFeatures::has_pcid) return;
            BitmapClear(pm->cpus_with_tlb, this_cpu()->id);
        }

        void ShootdownPage(pagemap_t *pm, uint64_t vaddr) {
            if (!pm) return;
            LocalInvlpg(pm, vaddr);
            if (!smp_started) return;
            uint32_t me = this_cpu()->id;

            uint64_t targets[TLB_MASK_WORDS];
            for (int i = 0; i < TLB_MASK_WORDS; i++)
                targets[i] = __atomic_load_n(&pm->cpus_with_tlb[i], __ATOMIC_RELAXED);
            targets[me / 64] &= ~(1ULL << (me % 64));

            for (int w = 0; w < TLB_MASK_WORDS; w++) {
                while (targets[w]) {
                    int b = __builtin_ffsll(targets[w]) - 1;
                    targets[w] &= ~(1ULL << b);
                    uint32_t i = w * 64 + b;
                    if (i >= MAX_CPU) continue;
                    cpu_t *target = smp_cpu_list[i];
                    if (!target) continue;

                    uint64_t rf;
                    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rf) :: "memory");
                    spinlock_lock(&target->shootdown_lock);
                    if (target->shootdown_count < 32) {
                        target->shootdown_queue[target->shootdown_count++] = {pm, vaddr, 1};
                    } else {
                        target->shootdown_queue[0] = {nullptr, 0, 3};
                        target->shootdown_count = 1;
                    }
                    spinlock_unlock(&target->shootdown_lock);
                    asm volatile("push %0\n\tpopfq" :: "r"(rf) : "memory");

                    LAPIC::IPI(target->lapic_id, TLB_FLUSH_VEC);
                }
            }
        }

        void ShootdownFull(pagemap_t *pm) {
            if (!pm) return;
            LocalFullFlush(pm);
            if (!smp_started) return;

            uint32_t me = this_cpu()->id;
            uint64_t targets[TLB_MASK_WORDS];
            for (int i = 0; i < TLB_MASK_WORDS; i++)
                targets[i] = __atomic_load_n(&pm->cpus_with_tlb[i], __ATOMIC_RELAXED);
            targets[me / 64] &= ~(1ULL << (me % 64));

            for (int w = 0; w < TLB_MASK_WORDS; w++) {
                while (targets[w]) {
                    int b = __builtin_ffsll(targets[w]) - 1;
                    targets[w] &= ~(1ULL << b);
                    uint32_t i = w * 64 + b;
                    if (i >= MAX_CPU) continue;
                    cpu_t *target = smp_cpu_list[i];
                    if (!target) continue;

                    uint64_t rf;
                    asm volatile("pushfq\n\tcli\n\tpop %0" : "=r"(rf) :: "memory");
                    spinlock_lock(&target->shootdown_lock);
                    if (target->shootdown_count < 32) {
                        target->shootdown_queue[target->shootdown_count++] = {pm, 0, 2};
                    } else {
                        target->shootdown_queue[0] = {nullptr, 0, 3};
                        target->shootdown_count = 1;
                    }
                    spinlock_unlock(&target->shootdown_lock);
                    asm volatile("push %0\n\tpopfq" :: "r"(rf) : "memory");

                    LAPIC::IPI(target->lapic_id, TLB_FLUSH_VEC);
                }
            }

            if (!CPUFeatures::has_pcid) {
                BitmapClearAll(pm->cpus_with_tlb);
                BitmapSet(pm->cpus_with_tlb, me);
            }
        }

        void IPIHandler(context_t * /*ctx*/) {
            cpu_t *c = this_cpu();
            if (!c) { LAPIC::EOI(); return; }

            while (true) {
                spinlock_lock(&c->shootdown_lock);
                if (c->shootdown_count == 0) {
                    spinlock_unlock(&c->shootdown_lock);
                    break;
                }
                auto req = c->shootdown_queue[--c->shootdown_count];
                spinlock_unlock(&c->shootdown_lock);

                if (req.type == 1) {
                    LocalInvlpg(req.pm, req.vaddr);
                } else if (req.type == 2) {
                    LocalFullFlush(req.pm);
                    if (req.pm) BitmapClear(req.pm->cpus_with_tlb, c->id);
                } else if (req.type == 3) {
                    LocalGlobalFlush();
                }
            }
            LAPIC::EOI();
        }
    }

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
            if (IsPM5LVL) {
                pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
                if (!PAGE_EXISTS(pml4)) return info;
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
                info.flags = pde_val & PTE_KEEP;                        // Reserve COW BIT
                return info;
            }

            uint64_t *pd = HIGHER_HALF(PTE_MASK(pde_val));
            uint64_t pte_val = pd[PDE(vaddr)];
            if (!PAGE_EXISTS(pte_val)) return info;
            if (pte_val & VMM_PS_BIT) {
                info.phys = pte_val & 0x000FFFFFFFE00000ULL;
                info.size = PAGE_2MB;
                info.flags = pte_val & PTE_KEEP;                        
                return info;
            }

            uint64_t *pt = HIGHER_HALF(PTE_MASK(pte_val));
            uint64_t page_val = pt[PTE(vaddr)];
            if (!PAGE_EXISTS(page_val)) return info;
            info.phys = page_val & 0x000FFFFFFFFFF000ULL;
            info.size = PAGE_SIZE;
            info.flags = page_val & PTE_KEEP;                          
            return info;
        }

        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags) {
            return VMM::VMA::InternalAlloc(pagemap, page_count, flags, 0);
        }
    }

    void Init(){
        VMM::LazyTLB::Init();

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
        kernel_pagemap->pt_lock = 0;
        kernel_pagemap->vma_lock = 0;                                   // OH! I forget THIS!!!!!
        kernel_pagemap->pcid = AllocPCID();
        BitmapClearAll(kernel_pagemap->cpus_with_tlb);
        rb_root_init(&kernel_pagemap->vma_tree, nullptr, nullptr, nullptr, nullptr, nullptr); 
        _memset(kernel_pagemap->toplvl, 0, PAGE_SIZE);

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
        if (IsPM5LVL) {
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

        pt[PTE(vaddr)] = (paddr & 0x000FFFFFFFFFF000ULL) | (flags & PTE_KEEP);   
    }

    void Map2M(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags){
        uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if (IsPM5LVL) {
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

        pd[PDE(vaddr)] = (paddr & 0x000FFFFFFFE00000ULL) | (flags & PTE_KEEP) | VMM_PS_BIT;
    }

    void Map1G(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags){
        uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if (IsPM5LVL) {
            pml4 = (uint64_t*)pagemap->toplvl[PML5E(vaddr)];
            if (!PAGE_EXISTS(pml4)) pml4 = VMM::Useless::NewLevel(pagemap->toplvl, PML5E(vaddr));
            pml4 = HIGHER_HALF(PTE_MASK(pml4));
        }
#endif
        uint64_t *pdpt = (uint64_t*)pml4[PML4E(vaddr)];
        if (!PAGE_EXISTS(pdpt)) pdpt = VMM::Useless::NewLevel(pml4, PML4E(vaddr));
        pdpt = HIGHER_HALF(PTE_MASK(pdpt));

        pdpt[PDPTE(vaddr)] = (paddr & 0x000FFFFFC0000000ULL) | (flags & PTE_KEEP) | VMM_PS_BIT;
    }

    void Map(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags){
        VMM::Map4K(pagemap, vaddr, paddr, flags);
    }

    void Map(uint64_t vaddr, uint64_t paddr){
        VMM::Map(kernel_pagemap, vaddr, paddr, MM_READ | MM_WRITE);
    }

    void UnmapNoFlush(pagemap_t *pagemap, uint64_t vaddr){
        uint64_t *pml4 = (uint64_t*)pagemap->toplvl;
#if CONFIG_VMM_5LVL_MAP == 1
        if (IsPM5LVL) {
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

    void Unmap(pagemap_t *pagemap, uint64_t vaddr){
        UnmapNoFlush(pagemap, vaddr);
        LazyTLB::ShootdownPage(pagemap, vaddr);
    }

    uint64_t GetPhysics(pagemap_t *pagemap, uint64_t vaddr){
        return VMM::Useless::GetPageInfo(pagemap, vaddr).phys;
    }

    void MapRange(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags, uint64_t count){
        uint64_t mapped = 0;
        while (mapped < count) {
            uint64_t cur_v = vaddr + mapped * PAGE_SIZE;
            uint64_t cur_p = paddr + mapped * PAGE_SIZE;
            uint64_t rem   = count - mapped;

            if ((cur_v & (PAGE_1GB - 1)) == 0 && (cur_p & (PAGE_1GB - 1)) == 0 && rem >= 262144) {
                VMM::Map1G(pagemap, cur_v, cur_p, flags);
                mapped += 262144; continue;
            }
            if ((cur_v & (PAGE_2MB - 1)) == 0 && (cur_p & (PAGE_2MB - 1)) == 0 && rem >= 512) {
                VMM::Map2M(pagemap, cur_v, cur_p, flags);
                mapped += 512; continue;
            }
            VMM::Map4K(pagemap, cur_v, cur_p, flags);
            mapped += 1;
        }
    }

    pagemap_t *SwitchPageMap(pagemap_t *pagemap){
        if (!pagemap) return nullptr;                        // Protect Empty Pointer

        uint64_t rflags;
        asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(rflags) :: "memory");

        pagemap_t *old_pagemap = nullptr;
        if (smp_started) {
            old_pagemap = this_cpu()->pagemap;
            this_cpu()->pagemap = pagemap;
        }

        uint64_t cr3_val = PHYSICAL((uint64_t)pagemap->toplvl);
        if (CPUFeatures::has_pcid && pagemap->pcid != 0) {
            cr3_val |= pagemap->pcid;
            cr3_val |= (1ULL << 63);
        }
        __asm__ volatile ("movq %0, %%cr3" : : "r"(cr3_val) : "memory");

        if (smp_started) {
            if (old_pagemap) LazyTLB::OnDetach(old_pagemap);
            LazyTLB::OnAttach(pagemap);
        }

        asm volatile("push %0\n\tpopfq" :: "r"(rflags) : "memory");
        return old_pagemap;
    }

    pagemap_t *NewPM(){
        pagemap_t *pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        pagemap->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        _memset(pagemap->toplvl, 0, PAGE_SIZE);
        pagemap->vm_mappings = nullptr;
        pagemap->vma_lock = 0;
        pagemap->pt_lock = 0;
        pagemap->vma_head = nullptr;
        pagemap->vma_cursor = nullptr;

        pagemap->pcid = AllocPCID();
        BitmapClearAll(pagemap->cpus_with_tlb);

        rb_root_init(&pagemap->vma_tree, nullptr, nullptr, nullptr, nullptr, nullptr);

        __memcpy(&pagemap->toplvl[256], &kernel_pagemap->toplvl[256], (512 - 256) * sizeof(uint64_t));

        VMM::VMA::SetStart(pagemap, HIGHER_HALF(0x100000000000), 0);
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

    void *Alloc(pagemap_t *pagemap, uint64_t page_count, bool user) {
        if (!page_count) return nullptr;
        uint64_t flags = MM_READ | MM_WRITE | (user ? MM_USER : 0);

        spinlock_lock(&pagemap->vma_lock);
        uint64_t addr = VMM::VMA::InternalAlloc(pagemap, page_count, flags, 0);
        if (!addr) { spinlock_unlock(&pagemap->vma_lock); return nullptr; }

        spinlock_lock(&pagemap->pt_lock);

        uint64_t mapped = 0;
        while (mapped < page_count) {
            uint64_t cv = addr + mapped * PAGE_SIZE;
            uint64_t rem = page_count - mapped;

            if (rem >= 524288 && (cv % PAGE_2GB) == 0) {
                void* phys_ptr = PMM::Request2GB();
                if (phys_ptr) {
                    uint64_t phys = (uint64_t)phys_ptr;
                    VMM::Map1G(pagemap, cv, phys, flags);
                    VMM::Map1G(pagemap, cv+PAGE_1GB, phys+PAGE_1GB, flags);
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
                goto err_unmap_alloc;
            }
            VMM::Map4K(pagemap, cv, (uint64_t)phys_ptr, flags);
            mapped += 1;
        }

        VMM::NewMapping(pagemap, addr, page_count, flags);
        spinlock_unlock(&pagemap->pt_lock);
        spinlock_unlock(&pagemap->vma_lock);
        return (void*)addr;

    err_unmap_alloc:
        uint64_t rollback_v = addr;
        uint64_t rollback_end = addr + mapped * PAGE_SIZE;
        while (rollback_v < rollback_end) {
            Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, rollback_v);
            if (info.size == 0) { rollback_v += PAGE_SIZE; continue; }

            if (info.size == PAGE_1GB) {
                Useless::PageInfo next = VMM::Useless::GetPageInfo(pagemap, rollback_v + PAGE_1GB);
                if (next.size == PAGE_1GB && next.phys == info.phys + PAGE_1GB) {
                    PMM::Free2GB((void*)info.phys);
                    VMM::UnmapNoFlush(pagemap, rollback_v);
                    VMM::UnmapNoFlush(pagemap, rollback_v + PAGE_1GB);
                    rollback_v += PAGE_2GB;
                } else {
                    for (uint32_t j = 0; j < 512; j++)
                        PMM::Free2MB((void*)(info.phys + j * PAGE_2MB));
                    VMM::UnmapNoFlush(pagemap, rollback_v);
                    rollback_v += PAGE_1GB;
                }
            } else if (info.size == PAGE_2MB) {
                PMM::Free2MB((void*)info.phys);
                VMM::UnmapNoFlush(pagemap, rollback_v);
                rollback_v += PAGE_2MB;
            } else {
                PMM::Free((void*)info.phys);
                VMM::UnmapNoFlush(pagemap, rollback_v);
                rollback_v += PAGE_SIZE;
            }
        }
        LazyTLB::ShootdownFull(pagemap);

        vma_region_t *region = VMM::VMA::FindRegion(pagemap, addr);
        if (region) VMM::VMA::RemoveRegion(pagemap, region);        // Add Empty Pointer Logic

        spinlock_unlock(&pagemap->pt_lock);
        spinlock_unlock(&pagemap->vma_lock);
        return nullptr;
    }

    void *EAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags) {
        if (!page_count) return nullptr;

        spinlock_lock(&pagemap->vma_lock);
        uint64_t addr = VMM::VMA::InternalAlloc(pagemap, page_count, flags, 0);
        if (!addr) { spinlock_unlock(&pagemap->vma_lock); return nullptr; }

        spinlock_lock(&pagemap->pt_lock);

        uint64_t mapped = 0;
        while (mapped < page_count) {
            uint64_t cv = addr + mapped * PAGE_SIZE;
            uint64_t rem = page_count - mapped;

            if (rem >= 524288 && (cv % PAGE_2GB) == 0) {
                void* phys_ptr = PMM::Request2GB();
                if (phys_ptr) {
                    uint64_t phys = (uint64_t)phys_ptr;
                    VMM::Map1G(pagemap, cv, phys, flags);
                    VMM::Map1G(pagemap, cv+PAGE_1GB, phys+PAGE_1GB, flags);
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
                goto err_unmap_ealloc;
            }
            VMM::Map4K(pagemap, cv, (uint64_t)phys_ptr, flags);
            mapped += 1;
        }

        VMM::NewMapping(pagemap, addr, page_count, flags);
        spinlock_unlock(&pagemap->pt_lock);
        spinlock_unlock(&pagemap->vma_lock);
        return (void*)addr;

    err_unmap_ealloc:
        uint64_t rollback_v = addr;
        uint64_t rollback_end = addr + mapped * PAGE_SIZE;
        while (rollback_v < rollback_end) {
            Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, rollback_v);
            if (info.size == 0) { rollback_v += PAGE_SIZE; continue; }

            if (info.size == PAGE_1GB) {
                Useless::PageInfo next = VMM::Useless::GetPageInfo(pagemap, rollback_v + PAGE_1GB);
                if (next.size == PAGE_1GB && next.phys == info.phys + PAGE_1GB) {
                    PMM::Free2GB((void*)info.phys);
                    VMM::UnmapNoFlush(pagemap, rollback_v);
                    VMM::UnmapNoFlush(pagemap, rollback_v + PAGE_1GB);
                    rollback_v += PAGE_2GB;
                } else {
                    for (uint32_t j = 0; j < 512; j++)
                        PMM::Free2MB((void*)(info.phys + j * PAGE_2MB));
                    VMM::UnmapNoFlush(pagemap, rollback_v);
                    rollback_v += PAGE_1GB;
                }
            } else if (info.size == PAGE_2MB) {
                PMM::Free2MB((void*)info.phys);
                VMM::UnmapNoFlush(pagemap, rollback_v);
                rollback_v += PAGE_2MB;
            } else {
                PMM::Free((void*)info.phys);
                VMM::UnmapNoFlush(pagemap, rollback_v);
                rollback_v += PAGE_SIZE;
            }
        }
        LazyTLB::ShootdownFull(pagemap);

        vma_region_t *region = VMM::VMA::FindRegion(pagemap, addr);
        if (region) VMM::VMA::RemoveRegion(pagemap, region);       

        spinlock_unlock(&pagemap->pt_lock);
        spinlock_unlock(&pagemap->vma_lock);
        return nullptr;
    }

    void Free(pagemap_t *pagemap, void *ptr){
        if (!pagemap || !ptr) return;                              
        if (((uint64_t)ptr & 0xfff) != 0) return;
        spinlock_lock(&pagemap->vma_lock);

        vma_region_t *region = VMM::VMA::FindRegion(pagemap, (uint64_t)ptr);
        if (!region || region->start != (uint64_t)ptr) {
            spinlock_unlock(&pagemap->vma_lock);
            return;
        }
        pagemap->vma_cursor = region->prev;

        spinlock_lock(&pagemap->pt_lock);

        uint64_t v = region->start;
        uint64_t end = v + region->page_count * PAGE_SIZE;
        while (v < end) {
            Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, v);
            if (info.size == 0) { v += PAGE_SIZE; continue; }

            if (info.size == PAGE_1GB) {
                Useless::PageInfo next = VMM::Useless::GetPageInfo(pagemap, v + PAGE_1GB);
                if (next.size == PAGE_1GB && next.phys == info.phys + PAGE_1GB) {
                    PMM::Free2GB((void*)info.phys);
                    VMM::UnmapNoFlush(pagemap, v);
                    VMM::UnmapNoFlush(pagemap, v + PAGE_1GB);
                    v += PAGE_2GB;
                } else {
                    for (uint32_t j = 0; j < 512; j++)
                        PMM::Free2MB((void*)(info.phys + j * PAGE_2MB));
                    VMM::UnmapNoFlush(pagemap, v);
                    v += PAGE_1GB;
                }
            } else if (info.size == PAGE_2MB) {
                PMM::Free2MB((void*)info.phys);
                VMM::UnmapNoFlush(pagemap, v);
                v += PAGE_2MB;
            } else {
                PMM::Free((void*)info.phys);
                VMM::UnmapNoFlush(pagemap, v);
                v += PAGE_SIZE;
            }
        }

        LazyTLB::ShootdownFull(pagemap);

        vm_mapping_t *m = pagemap->vm_mappings;
        if (m) {
            vm_mapping_t *start_m = m;
            do {
                if (m->start == (uint64_t)ptr) { RemoveMapping(m); break; }
                m = m->next;
            } while (m != start_m);
        }

        VMM::VMA::RemoveRegion(pagemap, region);

        spinlock_unlock(&pagemap->pt_lock);
        spinlock_unlock(&pagemap->vma_lock);
    }

    pagemap_t *Fork(pagemap_t *parent){
        pagemap_t *pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        pagemap->toplvl = HIGHER_HALF((uint64_t*)PMM::Request());
        _memset(pagemap->toplvl, 0, PAGE_SIZE);
        for (uint64_t i = 256; i < 512; i++)
            pagemap->toplvl[i] = kernel_pagemap->toplvl[i];

        pagemap->pt_lock = 0;
        pagemap->vma_lock = 0;                                   
        pagemap->vm_mappings = nullptr;                          
        pagemap->vma_head = nullptr;
        pagemap->vma_cursor = nullptr;
        pagemap->pcid = AllocPCID();
        BitmapClearAll(pagemap->cpus_with_tlb);
        rb_root_init(&pagemap->vma_tree, nullptr, nullptr, nullptr, nullptr, nullptr); 

        spinlock_lock(&parent->vma_lock);
        spinlock_lock(&parent->pt_lock);

        if (parent->vma_head) {                                   // Protect empty list
            VMM::VMA::SetStart(pagemap, parent->vma_head->start, 0);
            vma_region_t *r = parent->vma_head;
            do {
                if (r->start >= HIGHER_HALF(0)) { r = r->next; continue; }

                uint64_t v = r->start, mapped = 0;
                while (mapped < r->page_count) {
                    Useless::PageInfo info = VMM::Useless::GetPageInfo(parent, v);
                    if (info.size == 0) break;

                    uint64_t nf = (info.flags & ~MM_WRITE) | VMM_COW_BIT;
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
                r = r->next;
            } while (r != parent->vma_head);
        }

        LazyTLB::ShootdownFull(parent);

        spinlock_unlock(&parent->pt_lock);
        spinlock_unlock(&parent->vma_lock);

        return pagemap;
    }

    void CleanPM(pagemap_t *pagemap){
        if (!pagemap) return;                                    
        spinlock_lock(&pagemap->vma_lock);
        spinlock_lock(&pagemap->pt_lock);

        if (pagemap->vma_head) {                                  // Empty list protect
            vma_region_t *r = pagemap->vma_head->next;
            while (r != pagemap->vma_head) {
                vma_region_t *next = r->next;
                if (r->start < HIGHER_HALF(0)) {
                    uint64_t v = r->start, end = v + r->page_count * PAGE_SIZE;
                    while (v < end) {
                        Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, v);
                        if (info.size) {
                            if (info.size == PAGE_1GB) {
                                Useless::PageInfo n = VMM::Useless::GetPageInfo(pagemap, v + PAGE_1GB);
                                if (n.size == PAGE_1GB && n.phys == info.phys + PAGE_1GB) {
                                    PMM::Free2GB((void*)info.phys);
                                    VMM::UnmapNoFlush(pagemap, v);
                                    VMM::UnmapNoFlush(pagemap, v + PAGE_1GB);
                                    v += PAGE_2GB;
                                } else {
                                    for (uint32_t j = 0; j < 512; j++)
                                        PMM::Free2MB((void*)(info.phys + j * PAGE_2MB));
                                    VMM::UnmapNoFlush(pagemap, v);
                                    v += PAGE_1GB;
                                }
                            } else if (info.size == PAGE_2MB) {
                                PMM::Free2MB((void*)info.phys);
                                VMM::UnmapNoFlush(pagemap, v);
                                v += PAGE_2MB;
                            } else {
                                PMM::Free((void*)info.phys);
                                VMM::UnmapNoFlush(pagemap, v);
                                v += PAGE_SIZE;
                            }
                        } else {
                            v += PAGE_SIZE;
                        }
                    }
                }
                VMM::VMA::RemoveRegion(pagemap, r);
                r = next;
            }
            PMM::Free(PHYSICAL(pagemap->vma_head));
            pagemap->vma_head = pagemap->vma_cursor = nullptr;
        }

        vm_mapping_t *mapping = pagemap->vm_mappings;
        if (mapping) {
            size_t map_count = 0;
            vm_mapping_t *curr = mapping;
            do {
                map_count++;
                curr = curr->next;
            } while (curr != mapping);

            curr = mapping;
            for (size_t i = 0; i < map_count; i++) {
                vm_mapping_t *next = curr->next;
                VMM::RemoveMapping(curr);
                curr = next;
            }
        }
        pagemap->vm_mappings = nullptr;

        LazyTLB::ShootdownFull(pagemap);

        spinlock_unlock(&pagemap->pt_lock);
        spinlock_unlock(&pagemap->vma_lock);
    }

    static void FreePageTablesInternal(uint64_t *table, int level) {
        for (int i = 0; i < 256; i++) {          // 只走低半区，高半区与内核共享不可释放
            uint64_t entry = table[i];
            if (!(entry & 0x1)) continue;

            if (entry & VMM_PS_BIT) continue;

            uint64_t *child = HIGHER_HALF(PTE_MASK(entry));
            if (level > 1) {
                FreePageTablesInternal(child, level - 1);
                PMM::Free(PHYSICAL(child));
            }
        }
    }

    void DestroyPM(pagemap_t *pagemap){
        if (!pagemap) return;                                     

        VMM::CleanPM(pagemap);

        int start_level = IsPM5LVL ? 5 : 4;
        FreePageTablesInternal(pagemap->toplvl, start_level);

        FreePCID(pagemap->pcid);
        PMM::Free(PHYSICAL(pagemap->toplvl));
        PMM::Free(PHYSICAL(pagemap));
    }

    uint32_t HandlePF(context_t *ctx){
        uint64_t cr2 = 0;
        __asm__ volatile ("movq %%cr2, %0" : "=r"(cr2));
        uint64_t ec = ctx->error_code;

        bool p  = ec & (1 << 0);       // P    页存在（违规）还是缺页
        bool wr = ec & (1 << 1);       // W    写故障
        bool us = ec & (1 << 2);       // U    用户态
        bool id = ec & (1 << 4);       // ID   取指故障（NX）

        thread_t *t = Schedule::this_thread();
        if (!smp_started || !this_cpu() || !t) {
            kerror("[#PF] addr=%p rip=%p ec=%#lx : no thread context, cannot recover.\n",
                   cr2, ctx->rip, ec);
            return 1;
        }
        pagemap_t *pagemap = t->pagemap;
        if (!pagemap) return 1;

        // 重复故障熔断
        static uint64_t rf_cr2[MAX_CPU];
        static uint64_t rf_rip[MAX_CPU];
        static uint32_t rf_cnt[MAX_CPU];
        uint32_t cid = this_cpu()->id;
        if (cid >= MAX_CPU) cid = 0;
        if (rf_cr2[cid] == cr2 && rf_rip[cid] == ctx->rip) {
            if (++rf_cnt[cid] >= 3) {
                kerrorln("[#PF] repeated fault (addr=%p rip=%p) x3 -> kill thread",
                         cr2, ctx->rip);
                rf_cnt[cid] = 0;
                return 1;
            }
        } else {
            rf_cr2[cid] = cr2;
            rf_rip[cid] = ctx->rip;
            rf_cnt[cid] = 1;
        }

        uint64_t fault_addr = ALIGN_DOWN(cr2, PAGE_SIZE);
        spinlock_lock(&pagemap->pt_lock);
        Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, fault_addr);

        kinfoln("[#PF] addr=%p rip=%p ec=%#lx [%s%s%s%s] PTE{U=%d W=%d CoW=%d NX=%d size=%#lx}",
                cr2, ctx->rip, ec,
                p ? "P" : "np", wr ? "W" : "R", us ? "U" : "K", id ? " FETCH" : "",
                !!(info.flags & MM_USER), !!(info.flags & MM_WRITE),
                !!(info.flags & VMM_COW_BIT), !!(info.flags & MM_NX), info.size);

        auto segv = [&](const char *why) -> uint32_t {
            spinlock_unlock(&pagemap->pt_lock);
            kerrorln("Segmentation fault: %s (addr=%p rip=%p)", why, cr2, ctx->rip);
            return 1;   // ISR 必须终止该线程
        };

        if (info.size == 0)
            return segv("unmapped address (no demand paging)");
        if (id)
            return segv("instruction fetch on NX page");
        if (us && !(info.flags & MM_USER))
            return segv("user access to supervisor page");
        if (!wr)
            return segv("read fault on present page");

        /* ---------- 写故障 ---------- */
        if (!(info.flags & VMM_COW_BIT)) {
            if (info.flags & MM_WRITE) {
                // 页表已可写却 fault：本地陈旧 TLB。冲刷后重试（熔断器兜底）
                LazyTLB::LocalInvlpg(pagemap, fault_addr);
                spinlock_unlock(&pagemap->pt_lock);
                return 0;
            }
            return segv("write to read-only page");
        }

        /* ---------- CoW ---------- */
        uint64_t new_flags = (info.flags & ~VMM_COW_BIT) | MM_WRITE;

        if (info.size == PAGE_1GB) {
            /* 1G 大页：拆成 512 个 2M —— 511 个保持共享 + CoW，
               只有故障的那一个 2M 复制断开。
               原实现 memcpy 了整整 1GB 还把父进程的共享页全 free 掉了 */
            uint64_t base = fault_addr & ~(PAGE_1GB - 1);
            uint64_t sub  = fault_addr & (PAGE_1GB - 1) & ~(PAGE_2MB - 1);

            VMM::UnmapNoFlush(pagemap, base);
            for (uint64_t off = 0; off < PAGE_1GB; off += PAGE_2MB) {
                if (off == sub) continue;
                VMM::Map2M(pagemap, base + off, info.phys + off, info.flags); // 保留 CoW
            }
            uint64_t new_phys = (uint64_t)PMM::Request2MB();
            if (!new_phys) {
                spinlock_unlock(&pagemap->pt_lock);
                kerrorln("OOM during 1G CoW split");
                return 1;
            }
            __memcpy(HIGHER_HALF((void*)new_phys),
                     HIGHER_HALF((void*)(info.phys + sub)), PAGE_2MB);
            VMM::Map2M(pagemap, base + sub, new_phys, new_flags);
            LazyTLB::ShootdownPage(pagemap, base);   // 冲掉旧的 1G TLB 项
        } else if (info.size == PAGE_2MB) {
            uint64_t base = fault_addr & ~(PAGE_2MB - 1);
            uint64_t new_phys = (uint64_t)PMM::Request2MB();
            if (!new_phys) {
                spinlock_unlock(&pagemap->pt_lock);
                kerrorln("OOM during 2M CoW");
                return 1;
            }
            __memcpy(HIGHER_HALF((void*)new_phys),
                     HIGHER_HALF((void*)info.phys), PAGE_2MB);
            VMM::Map2M(pagemap, base, new_phys, new_flags);
            // 老页不释放：共享引用计数尚未实现（与原实现一致）
            LazyTLB::ShootdownPage(pagemap, base);
        } else {
            uint64_t new_phys = (uint64_t)PMM::Request();
            if (!new_phys) {
                spinlock_unlock(&pagemap->pt_lock);
                kerrorln("OOM during 4K CoW");
                return 1;
            }
            __memcpy(HIGHER_HALF((void*)new_phys),
                     HIGHER_HALF((void*)info.phys), PAGE_SIZE);
            VMM::Map4K(pagemap, fault_addr, new_phys, new_flags);
            LazyTLB::ShootdownPage(pagemap, fault_addr);
        }

        spinlock_unlock(&pagemap->pt_lock);
        return 0;   // 页表已修好，可以重试指令
    }
}