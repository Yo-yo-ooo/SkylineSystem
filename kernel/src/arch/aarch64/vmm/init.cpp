//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only

#include <arch/aarch64/vmm/vmm.h>
#include <arch/aarch64/asm/regs.h>
#include <mem/pmm.h>
#include <klib/klib.h>
#include <conf.h>
#include <limine.h>

// ==================== Limine 分页模式请求 ====================
#if CONFIG_VMM_5LVL_MAP == 1
#define REQ_TOP_LVL LIMINE_PAGING_MODE_AARCH64_5LVL
#else
#define REQ_TOP_LVL LIMINE_PAGING_MODE_AARCH64_4LVL
#endif

__attribute__((used, section(".limine_requests")))
static volatile struct limine_paging_mode_request paging_mode_request = {
    .id = LIMINE_PAGING_MODE_REQUEST_ID,
    .revision = 0,
    .mode = REQ_TOP_LVL,
    .max_mode = REQ_TOP_LVL,
    .min_mode = LIMINE_PAGING_MODE_AARCH64_MIN
};

// AArch64 页表属性位定义
#define PTE_VALID   (1ULL << 0)
#define PTE_TABLE   (1ULL << 1)  // 在 L-1/L0/L1/L2 表示下一级是页表，在 L3 必须为 1
#define PTE_PAGE    (1ULL << 1)  // L3 页描述符
#define PTE_BLOCK   (0ULL << 1)  // L0/L1/L2 块描述符

#define PTE_AF      (1ULL << 10) // Access Flag, 必须置 1 否则触发数据中止
#define PTE_SH_IS   (3ULL << 8)  // Inner Shareable

#define PTE_AP_EL0  (1ULL << 6)  // 允许 EL0 访问
#define PTE_AP_RO   (1ULL << 7)  // 只读
#define PTE_UXN     (1ULL << 53) // User Execute Never
#define PTE_PXN     (1ULL << 54) // Privileged Execute Never

// 软件自定义位：用于 CoW (Copy-on-Write)
#define PTE_COW     (1ULL << 55) 

// 内存类型索引 (对应 MAIR_EL1 中的配置)
#define MT_NORMAL_WB    0
#define MT_DEVICE_NGNRE 1
#define MT_NORMAL_WT    2
#define MT_NORMAL_NC    3

// 页面大小常量
#define PAGE_SIZE   4096
#define PAGE_2MB    (512 * PAGE_SIZE)
#define PAGE_1GB    (512 * PAGE_2MB)

// ==================== ARMv8 标准页表索引提取 ====================
// 5 级分页 (52位) 独有 Level -1
#define L_MINUS1_INDEX(v) (((v) >> 48) & 0xF)    // 仅 4 位
// 4 级/5 级分页共有级别
#define L0_INDEX(v) (((v) >> 39) & 0x1FF)
#define L1_INDEX(v) (((v) >> 30) & 0x1FF)
#define L2_INDEX(v) (((v) >> 21) & 0x1FF)
#define L3_INDEX(v) (((v) >> 12) & 0x1FF)

// 判断是否为内核高地址 (TTBR1 区域)
// 48位时看 bit 47，52位时看 bit 51
static inline bool IS_HIGHER_HALF(uint64_t v) {
    return v & (1ULL << (VMM::IsPM5LVL ? 51 : 47));
}

#define HIGHER_HALF(x) ((x) + 0xFFFF000000000000ULL) 
#define PHYSICAL(x)    ((x) - 0xFFFF000000000000ULL)

extern "C" void mmu_invlpg(uint64_t vaddr) {
    asm volatile("tlbi vaae1is, %0" :: "r"(vaddr >> 12) : "memory");
}

bool smp_started = false;

namespace VMM {
    volatile bool IsPM5LVL = false; // 默认 4 级分页

    // ==================== 动态探测分页模式 ====================
    void DetectPagingMode() {
        if (paging_mode_request.response != NULL) {
            IsPM5LVL = (paging_mode_request.response->mode == LIMINE_PAGING_MODE_AARCH64_5LVL);
        } else {
            IsPM5LVL = false; // 回退
        }
        
        if (IsPM5LVL){
            kinfoln("VMM: Running in 5-Level Paging (52-bit VA) Mode.");
        }else{
            kinfoln("VMM: Running in 4-Level Paging (48-bit VA) Mode.");
        }
    }

    namespace Useless {
        uint64_t *NewLevel(uint64_t *level, uint64_t entry) {
            uint64_t *new_level = PMM::Request();
            _memset(HIGHER_HALF(new_level), 0, PAGE_SIZE);
            level[entry] = (uint64_t)new_level | PTE_VALID | PTE_TABLE; 
            return HIGHER_HALF(new_level);
        }

        uint64_t *GetRoot(pagemap_t *pagemap, uint64_t vaddr) {
            return IS_HIGHER_HALF(vaddr) ? (uint64_t*)pagemap->toplvl[1] : (uint64_t*)pagemap->toplvl[0];
        }

        uint64_t FlagsToPTE(uint64_t flags) {
            uint64_t pte_flags = PTE_AF | PTE_SH_IS;
            if (!(flags & MM_WRITE)) pte_flags |= PTE_AP_RO;
            if (flags & MM_USER)     pte_flags |= PTE_AP_EL0;
            if (flags & MM_NX)       pte_flags |= PTE_UXN | PTE_PXN;
            return pte_flags;
        }

        PageInfo GetPageInfo(pagemap_t *pagemap, uint64_t vaddr) {
            PageInfo info = {0, 0, 0};
            uint64_t *l0_tbl = GetRoot(pagemap, vaddr);
            if (!l0_tbl) return info;

            // 动态 5 级分页处理：额外下钻 Level -1
            if (IsPM5LVL) {
                uint64_t l_m1_val = l0_tbl[L_MINUS1_INDEX(vaddr)];
                if (!(l_m1_val & PTE_VALID)) return info;
                l0_tbl = HIGHER_HALF(l_m1_val & 0x000FFFFFFFFFF000ULL);
            }

            uint64_t l0_val = l0_tbl[L0_INDEX(vaddr)];
            if (!(l0_val & PTE_VALID)) return info;
            // 1GB Block
            if (!(l0_val & PTE_TABLE)) {
                info.phys = l0_val & 0x000FFFFFC0000000ULL;
                info.size = PAGE_1GB;
                info.flags = l0_val;
                return info;
            }
            uint64_t *l1_tbl = HIGHER_HALF(l0_val & 0x000FFFFFFFFFF000ULL);

            uint64_t l1_val = l1_tbl[L1_INDEX(vaddr)];
            if (!(l1_val & PTE_VALID)) return info;
            // 2MB Block
            if (!(l1_val & PTE_TABLE)) {
                info.phys = l1_val & 0x000FFFFFE00000ULL;
                info.size = PAGE_2MB;
                info.flags = l1_val;
                return info;
            }
            uint64_t *l2_tbl = HIGHER_HALF(l1_val & 0x000FFFFFFFFFF000ULL);

            uint64_t l2_val = l2_tbl[L2_INDEX(vaddr)];
            if (!(l2_val & PTE_VALID)) return info;
            if (!(l2_val & PTE_TABLE)) return info; // L2 不支持块，必须是表
            uint64_t *l3_tbl = HIGHER_HALF(l2_val & 0x000FFFFFFFFFF000ULL);

            uint64_t l3_val = l3_tbl[L3_INDEX(vaddr)];
            if (!(l3_val & PTE_VALID)) return info;
            // 4KB Page
            info.phys = l3_val & 0x000FFFFFFFFFF000ULL;
            info.size = PAGE_SIZE;
            info.flags = l3_val;
            return info;
        }

        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags) {
            vma_region_t *region = pagemap->vma_head->next;
            uint64_t addr = 0;
            if (region == pagemap->vma_head) {
                addr = region->start + (region->page_count * PAGE_SIZE);
                VMM::VMA::AddRegion(pagemap, addr, page_count, flags);
                return addr;
            }
            for (; region != pagemap->vma_head; region = region->next) {
                if (region->next == pagemap->vma_head) {
                    addr = region->start + (region->page_count * PAGE_SIZE);
                    VMM::VMA::AddRegion(pagemap, addr, page_count, flags);
                    return addr;
                }
                uint64_t region_end = region->start + (region->page_count * PAGE_SIZE);
                if (region->next->start >= region_end + (page_count * PAGE_SIZE)) {
                    addr = region_end;
                    region = VMM::VMA::InsertRegion(region, addr, page_count, flags);
                    return addr;
                }
            }
            return 0;
        }
    } // namespace Useless

    // ==================== TCR/MAIR/SCTLR 硬件初始化 ====================
    static void setup_mair() {
        uint64_t mair = 0;
        mair |= (0xFFULL << (MT_NORMAL_WB * 8));    
        mair |= (0x00ULL << (MT_DEVICE_NGNRE * 8)); 
        mair |= (0xBBULL << (MT_NORMAL_WT * 8));    
        mair |= (0x44ULL << (MT_NORMAL_NC * 8));    
        asm volatile("msr mair_el1, %0" :: "r"(mair));
    }

    static void setup_tcr() {
        uint64_t tcr = 0;
        // 根据分页模式动态配置 T0SZ / T1SZ
        // 4 级 (48位): T0SZ = 16 (64-48=16)
        // 5 级 (52位): T0SZ = 12 (64-52=12)
        uint64_t tsz = IsPM5LVL ? 12 : 16;
        
        tcr |= (tsz << 0);  // T0SZ
        tcr |= (tsz << 16); // T1SZ
        tcr |= (2ULL << 12);  // IRGN0 (Normal, Outer)
        tcr |= (2ULL << 8);   // ORGN0 (Normal, Outer)
        tcr |= (3ULL << 28);  // IRGN1
        tcr |= (3ULL << 24);  // ORGN1
        tcr |= (2ULL << 14);  // TG0 = 4KB
        tcr |= (2ULL << 30);  // TG1 = 4KB
        tcr |= (1ULL << 20);  // TBI0 (Top Byte Ignore for userspace)
        asm volatile("msr tcr_el1, %0" :: "r"(tcr));
    }

    static void enable_mmu() {
        uint64_t sctlr;
        asm volatile("isb");
        asm volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
        sctlr |= (1ULL << 0);  // M
        sctlr |= (1ULL << 12); // I
        sctlr |= (1ULL << 2);  // C
        sctlr &= ~(1ULL << 1); // A
        asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));
        asm volatile("isb");
    }

    volatile pagemap_t* kernel_pagemap = nullptr;

    void Init() {
        DetectPagingMode();
        setup_mair();
        setup_tcr();
        kpokln("Setup MAIR/TCR for AArch64!");

        kernel_pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        kernel_pagemap->toplvl[0] = HIGHER_HALF(PMM::Request()); // User root (TTBR0)
        kernel_pagemap->toplvl[1] = HIGHER_HALF(PMM::Request()); // Kernel root (TTBR1)
        kernel_pagemap->vma_head = nullptr;
        
        _memset(kernel_pagemap->toplvl[0], 0, PAGE_SIZE);
        _memset(kernel_pagemap->toplvl[1], 0, PAGE_SIZE);

        // 用户态起点
        VMM::VMA::SetStart(kernel_pagemap, 0x100000000000, 1); 

        // TODO: 在此处映射 Limine 提供的内核段、Framebuffer 等
        // 可使用 VMM::Map4K / Map2M 映射对应段

        VMM::SwitchPageMap(kernel_pagemap);
        enable_mmu();
        kpokln("AArch64 VMM Initialized & MMU Enabled!");
    }

    // ==================== 动态感知映射函数 ====================
    void Map4K(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
        uint64_t *l0_tbl = VMM::Useless::GetRoot(pagemap, vaddr);
        
        if (IsPM5LVL) {
            uint64_t l_m1_val = l0_tbl[L_MINUS1_INDEX(vaddr)];
            if (!(l_m1_val & PTE_VALID)) l0_tbl = VMM::Useless::NewLevel(l0_tbl, L_MINUS1_INDEX(vaddr));
            else l0_tbl = HIGHER_HALF(l_m1_val & 0x000FFFFFFFFFF000ULL);
        }

        uint64_t l0_val = l0_tbl[L0_INDEX(vaddr)];
        uint64_t *l1_tbl;
        if (!(l0_val & PTE_VALID)) l1_tbl = VMM::Useless::NewLevel(l0_tbl, L0_INDEX(vaddr));
        else l1_tbl = HIGHER_HALF(l0_val & 0x000FFFFFFFFFF000ULL);

        uint64_t l1_val = l1_tbl[L1_INDEX(vaddr)];
        uint64_t *l2_tbl;
        if (!(l1_val & PTE_VALID)) l2_tbl = VMM::Useless::NewLevel(l1_tbl, L1_INDEX(vaddr));
        else l2_tbl = HIGHER_HALF(l1_val & 0x000FFFFFFFFFF000ULL);

        uint64_t l2_val = l2_tbl[L2_INDEX(vaddr)];
        uint64_t *l3_tbl;
        if (!(l2_val & PTE_VALID)) l3_tbl = VMM::Useless::NewLevel(l2_tbl, L2_INDEX(vaddr));
        else l3_tbl = HIGHER_HALF(l2_val & 0x000FFFFFFFFFF000ULL);

        l3_tbl[L3_INDEX(vaddr)] = (paddr & 0x000FFFFFFFFFF000ULL) | PTE_VALID | PTE_PAGE | VMM::Useless::FlagsToPTE(flags);
    }

    void Map2M(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
        uint64_t *l0_tbl = VMM::Useless::GetRoot(pagemap, vaddr);
        
        if (IsPM5LVL) {
            uint64_t l_m1_val = l0_tbl[L_MINUS1_INDEX(vaddr)];
            if (!(l_m1_val & PTE_VALID)) l0_tbl = VMM::Useless::NewLevel(l0_tbl, L_MINUS1_INDEX(vaddr));
            else l0_tbl = HIGHER_HALF(l_m1_val & 0x000FFFFFFFFFF000ULL);
        }

        uint64_t l0_val = l0_tbl[L0_INDEX(vaddr)];
        uint64_t *l1_tbl;
        if (!(l0_val & PTE_VALID)) l1_tbl = VMM::Useless::NewLevel(l0_tbl, L0_INDEX(vaddr));
        else l1_tbl = HIGHER_HALF(l0_val & 0x000FFFFFFFFFF000ULL);

        // 2MB Block 在 Level 1
        l1_tbl[L1_INDEX(vaddr)] = (paddr & 0x000FFFFFE00000ULL) | PTE_VALID | PTE_BLOCK | VMM::Useless::FlagsToPTE(flags);
    }

    void Map1G(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
        uint64_t *l0_tbl = VMM::Useless::GetRoot(pagemap, vaddr);
        
        if (IsPM5LVL) {
            uint64_t l_m1_val = l0_tbl[L_MINUS1_INDEX(vaddr)];
            if (!(l_m1_val & PTE_VALID)) l0_tbl = VMM::Useless::NewLevel(l0_tbl, L_MINUS1_INDEX(vaddr));
            else l0_tbl = HIGHER_HALF(l_m1_val & 0x000FFFFFFFFFF000ULL);
        }

        // 1GB Block 在 Level 0
        l0_tbl[L0_INDEX(vaddr)] = (paddr & 0x000FFFFFC0000000ULL) | PTE_VALID | PTE_BLOCK | VMM::Useless::FlagsToPTE(flags);
    }

    void Map(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
        VMM::Map4K(pagemap, vaddr, paddr, flags);
    }

    void Unmap(pagemap_t *pagemap, uint64_t vaddr) {
        Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, vaddr);
        if (info.size == 0) return;

        uint64_t *l0_tbl = VMM::Useless::GetRoot(pagemap, vaddr);
        if (IsPM5LVL) {
            uint64_t l_m1_val = l0_tbl[L_MINUS1_INDEX(vaddr)];
            if (!(l_m1_val & PTE_VALID)) return;
            l0_tbl = HIGHER_HALF(l_m1_val & 0x000FFFFFFFFFF000ULL);
        }

        if (info.size == PAGE_1GB) {
            l0_tbl[L0_INDEX(vaddr)] = 0;
        } else {
            uint64_t l0_val = l0_tbl[L0_INDEX(vaddr)];
            uint64_t *l1_tbl = HIGHER_HALF(l0_val & 0x000FFFFFFFFFF000ULL);
            if (info.size == PAGE_2MB) {
                l1_tbl[L1_INDEX(vaddr)] = 0;
            } else {
                uint64_t l1_val = l1_tbl[L1_INDEX(vaddr)];
                uint64_t *l2_tbl = HIGHER_HALF(l1_val & 0x000FFFFFFFFFF000ULL);
                uint64_t l2_val = l2_tbl[L2_INDEX(vaddr)];
                uint64_t *l3_tbl = HIGHER_HALF(l2_val & 0x000FFFFFFFFFF000ULL);
                l3_tbl[L3_INDEX(vaddr)] = 0;
            }
        }
    }

    uint64_t GetPhysics(pagemap_t *pagemap, uint64_t vaddr) {
        return VMM::Useless::GetPageInfo(pagemap, vaddr).phys;
    }

    void MapRange(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags, uint64_t count) {
        for (uint64_t i = 0; i < count; i++)
            VMM::Map4K(pagemap, vaddr + (i * PAGE_SIZE), paddr + (i * PAGE_SIZE), flags);
    }

    pagemap_t *SwitchPageMap(pagemap_t *pagemap) {
        asm volatile("msr daifset, #7");
        pagemap_t *old_pagemap = nullptr;
        /* if (smp_started) {
            old_pagemap = this_cpu()->pagemap;
            this_cpu()->pagemap = pagemap;
        } */
        asm volatile(
            "msr ttbr0_el1, %0\n"
            "msr ttbr1_el1, %1\n"
            "isb\n"
            "tlbi vmalle1is\n"
            :: "r"(PHYSICAL((uint64_t)pagemap->toplvl[0])), 
               "r"(PHYSICAL((uint64_t)pagemap->toplvl[1])) 
            : "memory"
        );
        asm volatile("msr daifclr, #7");
        return old_pagemap;
    } 

    pagemap_t *NewPM() {
        pagemap_t *pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        pagemap->toplvl[0] = HIGHER_HALF(PMM::Request());
        pagemap->toplvl[1] = HIGHER_HALF(PMM::Request());
        _memset(pagemap->toplvl[0], 0, PAGE_SIZE);
        _memset(pagemap->toplvl[1], 0, PAGE_SIZE);
        pagemap->vm_mappings = nullptr;
        pagemap->vma_lock = 0;
        pagemap->vma_head = nullptr;
        __memcpy(pagemap->toplvl[1], kernel_pagemap->toplvl[1], PAGE_SIZE);
        return pagemap;
    }

    // ==================== VMA & Alloc / Free / Fork ====================
    // (此部分代码与之前完全一致，保持不变)
    namespace VMA {
        void SetStart(pagemap_t *pagemap, uint64_t start, uint64_t page_count) {
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
            region->start = start; region->page_count = page_count;
            region->flags = MM_READ | MM_WRITE;
            region->next = region; region->prev = region;
            pagemap->vma_head = region;
        }
        vma_region_t *AddRegion(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags) {
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
            region->start = start; region->page_count = page_count; region->flags = flags;
            region->prev = pagemap->vma_head->prev; region->next = pagemap->vma_head;
            pagemap->vma_head->prev->next = region; pagemap->vma_head->prev = region;
            return region;
        }
        vma_region_t *InsertRegion(vma_region_t *after, uint64_t start, uint64_t page_count, uint64_t flags) {
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
            region->start = start; region->page_count = page_count; region->flags = flags;
            region->prev = after; region->next = after->next;
            after->next->prev = region; after->next = region;
            return region;
        }
        void RemoveRegion(vma_region_t *region) {
            region->next->prev = region->prev; region->prev->next = region->next;
            PMM::Free(PHYSICAL((void*)region));
        }
    }

    vm_mapping_t *NewMapping(pagemap_t *pagemap, uint64_t start, uint64_t page_count, uint64_t flags) {
        vm_mapping_t *mapping = HIGHER_HALF((vm_mapping_t*)PMM::Request());
        mapping->start = start; mapping->page_count = page_count; mapping->flags = flags;
        if (pagemap->vm_mappings) {
            mapping->prev = pagemap->vm_mappings->prev; mapping->next = pagemap->vm_mappings;
            pagemap->vm_mappings->prev->next = mapping; pagemap->vm_mappings->prev = mapping;
            return mapping;
        }
        mapping->prev = mapping->next = mapping;
        pagemap->vm_mappings = mapping;
        return mapping;
    }

    void RemoveMapping(vm_mapping_t *mapping) {
        mapping->next->prev = mapping->prev; mapping->prev->next = mapping->next;
        PMM::Free(PHYSICAL((void*)mapping));
    }

    void *Alloc(pagemap_t *pagemap, uint64_t page_count, bool user) {
        if (!page_count) return nullptr;
        uint64_t flags = MM_READ | MM_WRITE | (user ? MM_USER : 0);
        spinlock_lock(&pagemap->vma_lock);
        uint64_t addr = VMM::Useless::InternalAlloc(pagemap, page_count, flags);
        if (!addr) { spinlock_unlock(&pagemap->vma_lock); return nullptr; }

        uint64_t mapped = 0;
        while (mapped < page_count) {
            uint64_t current_vaddr = addr + mapped * PAGE_SIZE;
            uint64_t remaining = page_count - mapped;

            if (remaining >= 524288 && (current_vaddr % PAGE_2GB) == 0) {
                void* phys_ptr = PMM::Request2GB();
                if (phys_ptr) {
                    uint64_t phys = (uint64_t)phys_ptr;
                    VMM::Map1G(pagemap, current_vaddr, phys, flags);
                    VMM::Map1G(pagemap, current_vaddr + PAGE_1GB, phys + PAGE_1GB, flags);
                    mapped += 524288; continue;
                }
            }
            if (remaining >= 512 && (current_vaddr % PAGE_2MB) == 0) {
                void* phys_ptr = PMM::Request2MB();
                if (phys_ptr) {
                    VMM::Map2M(pagemap, current_vaddr, (uint64_t)phys_ptr, flags);
                    mapped += 512; continue;
                }
            }
            void* phys_ptr = PMM::Request();
            if (!phys_ptr) { kerrorln("PMM: Out of memory during Alloc!"); break; }
            VMM::Map4K(pagemap, current_vaddr, (uint64_t)phys_ptr, flags);
            mapped += 1;
        }
        VMM::NewMapping(pagemap, addr, page_count, flags);
        spinlock_unlock(&pagemap->vma_lock);
        return (void*)addr;
    }

    void Free(pagemap_t *pagemap, void *ptr) {
        if (((uint64_t)ptr & 0xfff) != 0) return;
        spinlock_lock(&pagemap->vma_lock);
        vma_region_t *region = pagemap->vma_head->next;
        for (; region != pagemap->vma_head; region = region->next) {
            if (region->start == (uint64_t)ptr) {
                uint64_t vaddr = region->start;
                uint64_t end = region->start + region->page_count * PAGE_SIZE;
                while (vaddr < end) {
                    Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, vaddr);
                    if (info.size == 0) { vaddr += PAGE_SIZE; continue; }
                    if (info.size == PAGE_1GB) {
                        Useless::PageInfo next_info = VMM::Useless::GetPageInfo(pagemap, vaddr + PAGE_1GB);
                        if (next_info.size == PAGE_1GB && next_info.phys == info.phys + PAGE_1GB) {
                            PMM::Free2GB((void*)info.phys);
                            VMM::Unmap(pagemap, vaddr + PAGE_1GB); mmu_invlpg(vaddr + PAGE_1GB);
                            vaddr += PAGE_2GB;
                        } else {
                            for(uint32_t j=0; j<512; j++) PMM::Free2MB((void*)info.phys);
                            vaddr += PAGE_1GB;
                        }
                    } else if (info.size == PAGE_2MB) {
                        PMM::Free2MB((void*)info.phys); vaddr += PAGE_2MB;
                    } else {
                        PMM::Free((void*)info.phys); vaddr += PAGE_SIZE;
                    }
                    VMM::Unmap(pagemap, vaddr - info.size); mmu_invlpg(vaddr - info.size);
                }
                VMM::VMA::RemoveRegion(region);
                spinlock_unlock(&pagemap->vma_lock);
                return;
            }
        }
        spinlock_unlock(&pagemap->vma_lock);
    }

    pagemap_t *Fork(pagemap_t *parent) {
        pagemap_t *restore = VMM::SwitchPageMap(kernel_pagemap);
        pagemap_t *pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        pagemap->toplvl[0] = HIGHER_HALF(PMM::Request());
        pagemap->toplvl[1] = HIGHER_HALF(PMM::Request());
        _memset(pagemap->toplvl[0], 0, PAGE_SIZE);
        _memset(pagemap->toplvl[1], 0, PAGE_SIZE);
        __memcpy(pagemap->toplvl[1], kernel_pagemap->toplvl[1], PAGE_SIZE);

        vm_mapping_t *mapping = parent->vm_mappings;
        if (mapping) {
            vm_mapping_t *start_mapping = mapping;
            bool cont = true;
            while (cont) {
                if (mapping->start < HIGHER_HALF(0)) {
                    uint64_t vaddr = mapping->start;
                    uint64_t mapped = 0;
                    while (mapped < mapping->page_count) {
                        Useless::PageInfo info = VMM::Useless::GetPageInfo(parent, vaddr);
                        if (info.size == 0) break;
                        uint64_t new_flags = info.flags & ~PTE_AP_RO;
                        new_flags &= ~PTE_COW;
                        new_flags |= PTE_COW;

                        if (info.size == PAGE_1GB) {
                            VMM::Map1G(pagemap, vaddr, info.phys, new_flags);
                            VMM::Map1G(parent, vaddr, info.phys, new_flags);
                            mapped += 262144;
                        } else if (info.size == PAGE_2MB) {
                            VMM::Map2M(pagemap, vaddr, info.phys, new_flags);
                            VMM::Map2M(parent, vaddr, info.phys, new_flags);
                            mapped += 512;
                        } else {
                            VMM::Map4K(pagemap, vaddr, info.phys, new_flags);
                            VMM::Map4K(parent, vaddr, info.phys, new_flags);
                            mapped += 1;
                        }
                        vaddr += info.size;
                    }
                    VMM::NewMapping(pagemap, mapping->start, mapping->page_count, mapping->flags);
                }
                mapping = mapping->next;
                if (mapping == start_mapping) cont = false;
            }
        }
        VMM::VMA::SetStart(pagemap, parent->vma_head->start, parent->vma_head->page_count);
        spinlock_lock(&parent->vma_lock);
        vma_region_t *region = parent->vma_head->next;
        for (; region != parent->vma_head; region = region->next) {
            if (region->start < HIGHER_HALF(0)) {
                VMM::VMA::AddRegion(pagemap, region->start, region->page_count, region->flags);
            }
        }
        spinlock_unlock(&parent->vma_lock);
        VMM::SwitchPageMap(restore);
        return pagemap;
    }

    void CleanPM(pagemap_t *pagemap) {
        vma_region_t *region = pagemap->vma_head->next;
        vma_region_t *next_region = region->next;
        for (; region != pagemap->vma_head; region = next_region) {
            next_region = region->next;
            uint64_t vaddr = region->start;
            uint64_t end = region->start + region->page_count * PAGE_SIZE;
            while(vaddr < end) {
                Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, vaddr);
                if(info.size) {
                    if(info.size == PAGE_2MB) PMM::Free2MB((void*)info.phys);
                    else if(info.size == PAGE_1GB) PMM::Free2GB((void*)info.phys);
                    else PMM::Free((void*)info.phys);
                    VMM::Unmap(pagemap, vaddr);
                }
                vaddr += (info.size ? info.size : PAGE_SIZE);
            }
            VMM::VMA::RemoveRegion(region);
        }
        PMM::Free(PHYSICAL(pagemap->vma_head));
        pagemap->vma_head = nullptr;

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

    void DestroyPM(pagemap_t *pagemap) {
        VMM::CleanPM(pagemap);
        PMM::Free(PHYSICAL(pagemap->toplvl[0])); // 释放 TTBR0
        PMM::Free(PHYSICAL(pagemap->toplvl[1])); // 释放 TTBR1
        PMM::Free(PHYSICAL(pagemap));
    }
/* 
    uint32_t HandlePF(context_t *ctx) {
        uint64_t far;
        asm volatile("mrs %0, far_el1" : "=r"(far));
        uint64_t esr;
        asm volatile("mrs %0, esr_el1" : "=r"(esr));
        bool write = ((esr >> 6) & 0x1) != 0;
        
        kinfoln("[#PF] Addr: 0x%p | RIP: 0x%p | Cause: %s", far, ctx->elr, write ? "(Write)" : "(Read)");

        thread_t *t = Schedule::this_thread();
        if (!smp_started || !this_cpu() || !t) {
            kerror("Page fault on 0x%p, Should NOT continue.\n", far);
            return 1;
        }

        pagemap_t *restore = VMM::SwitchPageMap(kernel_pagemap);
        uint64_t fault_addr = ALIGN_DOWN(far, PAGE_SIZE);
        pagemap_t *pagemap = t->pagemap;

        Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, fault_addr);
        uint64_t old_phys = info.phys;
        if (!old_phys) {
            kerrorln("Segmentation fault (core undumped)");
            VMM::SwitchPageMap(restore);
            return 1;
        }

        uint64_t new_flags = info.flags;
        new_flags &= ~PTE_COW;
        new_flags &= ~PTE_AP_RO;

        if (info.size == PAGE_1GB) {
            uint64_t page_start = fault_addr & ~(PAGE_1GB - 1);
            VMM::Unmap(pagemap, page_start);
            mmu_invlpg(page_start);
            for (int j = 0; j < 512; j++) {
                uint64_t v = page_start + j * PAGE_2MB;
                uint64_t p = old_phys + j * PAGE_2MB;
                uint64_t new_phys = (uint64_t)PMM::Request2MB();
                if (!new_phys) { kerrorln("OOM"); VMM::SwitchPageMap(restore); return 1; }
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

        mmu_invlpg(fault_addr);
        VMM::SwitchPageMap(restore);
        return 0;
    } */
} // namespace VMM