//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/aarch64/allin.h>
#include <arch/aarch64/vmm/vmm.h>
#include <arch/aarch64/asm/regs.h>
#include <mem/pmm.h>
#include <klib/klib.h>
#include <conf.h>

// AArch64 页表属性位定义
#define PTE_VALID   (1ULL << 0)
#define PTE_TABLE   (1ULL << 1)  // 在 L0/L1/L2 表示下一级是页表，在 L3 必须为 1
#define PTE_PAGE    (1ULL << 1)  // L3 页描述符
#define PTE_BLOCK   (0ULL << 1)  // L1/L2 块描述符

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

// 虚拟地址索引提取
#define L0_INDEX(v) (((v) >> 39) & 0x1FF)
#define L1_INDEX(v) (((v) >> 30) & 0x1FF)
#define L2_INDEX(v) (((v) >> 21) & 0x1FF)
#define L3_INDEX(v) (((v) >> 12) & 0x1FF)

// 判断是否为内核高地址 (TTBR1 区域)
#define IS_HIGHER_HALF(v) ((v) & (1ULL << 47))

#define HIGHER_HALF(x) ((x) + 0xFFFF000000000000ULL) // 假设的高位内核映射基址
#define PHYSICAL(x)    ((x) - 0xFFFF000000000000ULL)

extern "C" void mmu_invlpg(uint64_t vaddr) {
    asm volatile("tlbi vaae1is, %0" :: "r"(vaddr >> 12) : "memory");
}

extern bool smp_started;

namespace VMM {

    namespace Useless {
        // 通用：分配新页表并链接到上一级
        uint64_t *NewLevel(uint64_t *level, uint64_t entry) {
            uint64_t *new_level = PMM::Request();
            _memset(HIGHER_HALF(new_level), 0, PAGE_SIZE);
            // 表描述符: Valid + Table + 物理地址
            level[entry] = (uint64_t)new_level | PTE_VALID | PTE_TABLE; 
            return HIGHER_HALF(new_level);
        }

        // 获取根页表：用户态用 TTBR0，内核态用 TTBR1
        uint64_t *GetRoot(pagemap_t *pagemap, uint64_t vaddr) {
            return IS_HIGHER_HALF(vaddr) ? (uint64_t*)pagemap->toplvl[1] : (uint64_t*)pagemap->toplvl[0];
        }

        // 统一的标志位转换函数
        uint64_t FlagsToPTE(uint64_t flags) {
            uint64_t pte_flags = PTE_AF | PTE_SH_IS;
            if (!(flags & MM_WRITE)) pte_flags |= PTE_AP_RO;
            if (flags & MM_USER)     pte_flags |= PTE_AP_EL0;
            if (flags & MM_NX)       pte_flags |= PTE_UXN | PTE_PXN;
            return pte_flags;
        }

        PageInfo GetPageInfo(pagemap_t *pagemap, uint64_t vaddr) {
            PageInfo info = {0, 0, 0};
            uint64_t *l0 = GetRoot(pagemap, vaddr);
            if (!l0) return info;

            uint64_t l1_val = l0[L0_INDEX(vaddr)];
            if (!(l1_val & PTE_VALID)) return info;
            if (l1_val & PTE_TABLE) {
                uint64_t *l1 = HIGHER_HALF(l1_val & 0x000FFFFFFFFFF000ULL);
                uint64_t l2_val = l1[L1_INDEX(vaddr)];
                if (!(l2_val & PTE_VALID)) return info;
                if (l2_val & PTE_TABLE) {
                    uint64_t *l2 = HIGHER_HALF(l2_val & 0x000FFFFFFFFFF000ULL);
                    uint64_t l3_val = l2[L2_INDEX(vaddr)];
                    if (!(l3_val & PTE_VALID)) return info;
                    // 4KB Page
                    info.phys = l3_val & 0x000FFFFFFFFFF000ULL;
                    info.size = PAGE_SIZE;
                    info.flags = l3_val;
                    return info;
                }
                // 2MB Block
                info.phys = l2_val & 0x000FFFFFE00000ULL;
                info.size = PAGE_2MB;
                info.flags = l2_val;
                return info;
            }
            // 1GB Block
            info.phys = l1_val & 0x000FFFFFC0000000ULL;
            info.size = PAGE_1GB;
            info.flags = l1_val;
            return info;
        }

        uint64_t InternalAlloc(pagemap_t *pagemap, uint64_t page_count, uint64_t flags) {
            // 此处逻辑与 x86_64 完全一致
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
        mair |= (0xFFULL << (MT_NORMAL_WB * 8));    // Write-back
        mair |= (0x00ULL << (MT_DEVICE_NGNRE * 8)); // Device nGnRnE
        mair |= (0xBBULL << (MT_NORMAL_WT * 8));    // Write-through
        mair |= (0x44ULL << (MT_NORMAL_NC * 8));    // Non-cacheable
        asm volatile("msr mair_el1, %0" :: "r"(mair));
    }

    static void setup_tcr() {
        // TCR: TTBR0 48-bit, TTBR1 48-bit, 4KB granule
        uint64_t tcr = 0;
        tcr |= (16ULL << 0);  // T0SZ = 16 (48-bit space)
        tcr |= (16ULL << 16); // T1SZ = 16 (48-bit space)
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
        sctlr |= (1ULL << 0);  // M: Enable MMU
        sctlr |= (1ULL << 12); // I: Instruction cache
        sctlr |= (1ULL << 2);  // C: Data/Unified cache
        sctlr &= ~(1ULL << 1); // A: Disable alignment fault check
        asm volatile("msr sctlr_el1, %0" :: "r"(sctlr));
        asm volatile("isb");
    }

    // ==================== VMM Init ====================
    volatile pagemap_t* kernel_pagemap = nullptr;

    void Init() {
        setup_mair();
        setup_tcr();
        kpokln("Setup MAIR/TCR for AArch64!");

        kernel_pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        // AArch64 需要两个根页表: TTBR0 (User) 和 TTBR1 (Kernel)
        kernel_pagemap->toplvl[0] = HIGHER_HALF(PMM::Request()); // User root
        kernel_pagemap->toplvl[1] = HIGHER_HALF(PMM::Request()); // Kernel root
        kernel_pagemap->vma_head = nullptr;
        
        _memset(kernel_pagemap->toplvl[0], 0, PAGE_SIZE);
        _memset(kernel_pagemap->toplvl[1], 0, PAGE_SIZE);

        VMM::VMA::SetStart(kernel_pagemap, 0x100000000000, 1); // 用户态起点

        // TODO: 映射 Limine 提供的内核段、Framebuffer 等
        // 这里假设 Limine 已经映射了内核，我们只需接管页表。
        // 如果需要重新映射，请用 VMM::Map4K / Map2M 映射对应段。

        VMM::SwitchPageMap(kernel_pagemap);
        enable_mmu();
        kpokln("AArch64 VMM Initialized & MMU Enabled!");
    }

    // ==================== 映射函数 ====================
    void Map4K(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
        uint64_t *l0 = VMM::Useless::GetRoot(pagemap, vaddr);
        uint64_t *l1 = (uint64_t*)(l0[L0_INDEX(vaddr)] & 0x000FFFFFFFFFF000ULL);
        if (!l1 || !(l0[L0_INDEX(vaddr)] & PTE_VALID)) l1 = VMM::Useless::NewLevel(l0, L0_INDEX(vaddr));
        else l1 = HIGHER_HALF(l1);

        uint64_t *l2 = (uint64_t*)(l1[L1_INDEX(vaddr)] & 0x000FFFFFFFFFF000ULL);
        if (!l2 || !(l1[L1_INDEX(vaddr)] & PTE_VALID)) l2 = VMM::Useless::NewLevel(l1, L1_INDEX(vaddr));
        else l2 = HIGHER_HALF(l2);

        uint64_t *l3 = (uint64_t*)(l2[L2_INDEX(vaddr)] & 0x000FFFFFFFFFF000ULL);
        if (!l3 || !(l2[L2_INDEX(vaddr)] & PTE_VALID)) l3 = VMM::Useless::NewLevel(l2, L2_INDEX(vaddr));
        else l3 = HIGHER_HALF(l3);

        l3[L3_INDEX(vaddr)] = (paddr & 0x000FFFFFFFFFF000ULL) | PTE_VALID | PTE_PAGE | VMM::Useless::FlagsToPTE(flags);
    }

    void Map2M(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
        uint64_t *l0 = VMM::Useless::GetRoot(pagemap, vaddr);
        uint64_t *l1 = (uint64_t*)(l0[L0_INDEX(vaddr)] & 0x000FFFFFFFFFF000ULL);
        if (!l1 || !(l0[L0_INDEX(vaddr)] & PTE_VALID)) l1 = VMM::Useless::NewLevel(l0, L0_INDEX(vaddr));
        else l1 = HIGHER_HALF(l1);

        uint64_t *l2 = (uint64_t*)(l1[L1_INDEX(vaddr)] & 0x000FFFFFFFFFF000ULL);
        if (!l2 || !(l1[L1_INDEX(vaddr)] & PTE_VALID)) l2 = VMM::Useless::NewLevel(l1, L1_INDEX(vaddr));
        else l2 = HIGHER_HALF(l2);

        // 2MB Block
        l2[L2_INDEX(vaddr)] = (paddr & 0x000FFFFFE00000ULL) | PTE_VALID | PTE_BLOCK | VMM::Useless::FlagsToPTE(flags);
    }

    void Map1G(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
        uint64_t *l0 = VMM::Useless::GetRoot(pagemap, vaddr);
        uint64_t *l1 = (uint64_t*)(l0[L0_INDEX(vaddr)] & 0x000FFFFFFFFFF000ULL);
        if (!l1 || !(l0[L0_INDEX(vaddr)] & PTE_VALID)) l1 = VMM::Useless::NewLevel(l0, L0_INDEX(vaddr));
        else l1 = HIGHER_HALF(l1);

        // 1GB Block
        l1[L1_INDEX(vaddr)] = (paddr & 0x000FFFFFC0000000ULL) | PTE_VALID | PTE_BLOCK | VMM::Useless::FlagsToPTE(flags);
    }

    void Map(pagemap_t *pagemap, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
        VMM::Map4K(pagemap, vaddr, paddr, flags);
    }

    void Unmap(pagemap_t *pagemap, uint64_t vaddr) {
        Useless::PageInfo info = VMM::Useless::GetPageInfo(pagemap, vaddr);
        if (info.size == 0) return;

        uint64_t *l0 = VMM::Useless::GetRoot(pagemap, vaddr);
        uint64_t *l1 = HIGHER_HALF((uint64_t*)(l0[L0_INDEX(vaddr)] & 0x000FFFFFFFFFF000ULL));
        
        if (info.size == PAGE_1GB) {
            l1[L1_INDEX(vaddr)] = 0;
        } else {
            uint64_t *l2 = HIGHER_HALF((uint64_t*)(l1[L1_INDEX(vaddr)] & 0x000FFFFFFFFFF000ULL));
            if (info.size == PAGE_2MB) {
                l2[L2_INDEX(vaddr)] = 0;
            } else {
                uint64_t *l3 = HIGHER_HALF((uint64_t*)(l2[L2_INDEX(vaddr)] & 0x000FFFFFFFFFF000ULL));
                l3[L3_INDEX(vaddr)] = 0;
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

    // ==================== 上下文切换 ====================
    pagemap_t *SwitchPageMap(pagemap_t *pagemap) {
        asm volatile("msr daifset, #7"); // 禁止中断
        pagemap_t *old_pagemap = nullptr;
        if (smp_started) {
            old_pagemap = this_cpu()->pagemap;
            this_cpu()->pagemap = pagemap;
        }
        // AArch64 需要同时设置 TTBR0 和 TTBR1
        asm volatile(
            "msr ttbr0_el1, %0\n"
            "msr ttbr1_el1, %1\n"
            "isb\n"
            "tlbi vmalle1is\n" // 刷新整个 TLB
            :: "r"(PHYSICAL((uint64_t)pagemap->toplvl[0])), 
               "r"(PHYSICAL((uint64_t)pagemap->toplvl[1])) 
            : "memory"
        );
        asm volatile("msr daifclr, #7"); // 恢复中断
        return old_pagemap;
    }

    pagemap_t *NewPM() {
        pagemap_t *pagemap = HIGHER_HALF((pagemap_t*)PMM::Request());
        pagemap->toplvl[0] = HIGHER_HALF(PMM::Request()); // User
        pagemap->toplvl[1] = HIGHER_HALF(PMM::Request()); // Kernel
        
        _memset(pagemap->toplvl[0], 0, PAGE_SIZE);
        _memset(pagemap->toplvl[1], 0, PAGE_SIZE);
        
        pagemap->vm_mappings = nullptr;
        pagemap->vma_lock = 0;
        pagemap->vma_head = nullptr;
        
        // 共享内核空间 (TTBR1) 的顶层页表
        __memcpy(pagemap->toplvl[1], kernel_pagemap->toplvl[1], PAGE_SIZE);
        return pagemap;
    }

    // ==================== VMA & Alloc / Free / Fork ====================
    // 这部分与 x86_64 版本完全一致，无需任何修改！
    namespace VMA {
        void SetStart(pagemap_t *pagemap, uint64_t start, uint64_t page_count) {
            vma_region_t *region = HIGHER_HALF((vma_region_t*)PMM::Request());
            region->start = start;
            region->page_count = page_count;
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
    } // namespace VMA

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
        // 共享内核 TTBR1
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
                        uint64_t new_flags = info.flags & ~PTE_AP_RO; // 去掉只读
                        new_flags &= ~PTE_COW;
                        new_flags |= PTE_COW; // 标记为 CoW

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
        // 与 x86_64 完全一致
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

    // ==================== AArch64 缺页中断处理 ====================
    uint32_t HandlePF(context_t *ctx) {
        uint64_t far;
        asm volatile("mrs %0, far_el1" : "=r"(far)); // 读取引发缺页的虚拟地址

        // 解析 ESR_EL1 判断是读/写/取指
        uint64_t esr;
        asm volatile("mrs %0, esr_el1" : "=r"(esr));
        bool write = ((esr >> 6) & 0x1) != 0; // WnR 位: 1=Write, 0=Read
        
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
        new_flags &= ~PTE_COW;      // 清除 CoW 标志
        new_flags &= ~PTE_AP_RO;    // 清除只读标志，恢复可写

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
    }
} // namespace VMM