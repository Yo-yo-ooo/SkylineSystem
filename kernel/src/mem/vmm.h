#pragma once

#ifndef _VMM_H_
#define _VMM_H_

#include "../klib/klib.h"

#ifdef __x86_64__

#include "../arch/x86_64/interrupt/idt.h"

#define PTE_PRESENT (u64)1
#define PTE_WRITABLE (u64)2
#define PTE_USER (u64)4
#define PTE_NX (1ull << 63)

#define PTE_ADDR_MASK 0x000ffffffffff000
#define PTE_GET_ADDR(VALUE) ((VALUE) & PTE_ADDR_MASK)
#define PTE_GETES(VALUE) ((VALUE) & ~PTE_ADDR_MASK)

struct registers__;
typedef struct registers__ registers;

#elif defined (__loongarch64)

#define INVALID_PAGE    0

#define PTE_VALID   ((uint64_t)1 << 0)
#define PTE_DIRTY   ((uint64_t)1 << 1)
#define PTE_MAT_CC  ((uint64_t)1 << 4)
#define PTE_MAT_WUC ((uint64_t)1 << 5)
#define PTE_GLOBAL  ((uint64_t)1 << 6)
#define PTE_HUGE    ((uint64_t)1 << 6)
#define PTE_WRITE   ((uint64_t)1 << 8)
#define PTE_HGLOBAL ((uint64_t)1 << 12)
#define PTE_NX      ((uint64_t)1 << 62)
#define PT_PADDR_MASK   ((uint64_t)0x0000FFFFFFFFF000)
#define PT_PADDR_HMASK  ((uint64_t)0x0000FFFFFF000000)

#define PT_TABLEES      0
#define PT_IS_LARGE(x)      (((x) & (PTE_HGLOBAL | PTE_HUGE)) == (PTE_HGLOBAL | PTE_HUGE))

#define PTE_GET_ADDR(VALUE) ((VALUE) & PT_PADDR_MASK)
#define PTE_GETES(VALUE) ((VALUE) & ~PT_PADDR_MASK)

#elif defined (__aarch64__)

// Here we operate under the assumption that 4K pages are supported by the CPU.
// This appears to be guaranteed by UEFI, as section 2.3.6 "AArch64 Platforms"
// states that the primary processor core configuration includes 4K translation
// granules (TCR_EL1.TG0 = 0).
// Support for 4K pages also implies 2M, 1G and 512G blocks.

// Sanity check that 4K pages are supported.
void vmm_assert_4k_pages(void) {
    uint64_t aa64mmfr0;
    asm volatile ("mrs %0, id_aa64mmfr0_el1" : "=r"(aa64mmfr0));

    if (((aa64mmfr0 >> 28) & 0b1111) == 0b1111) {
        Panic(false, "vmm: CPU does not support 4K pages, please make a bug report about this.");
    }
}

#define PTE_VALID    ((uint64_t)1 << 0)
#define PTE_TABLE    ((uint64_t)1 << 1)
#define PTE_4K_PAGE  ((uint64_t)1 << 1)
#define PTE_BLOCK    ((uint64_t)0 << 1)
#define PTE_USER     ((uint64_t)1 << 6)
#define PTE_READONLY ((uint64_t)1 << 7)
#define PTE_INNER_SH ((uint64_t)3 << 8)
#define PTE_ACCESS   ((uint64_t)1 << 10)
#define PTE_XN       ((uint64_t)1 << 54)
#define PTE_WB       ((uint64_t)0 << 2)
#define PTE_FB       ((uint64_t)1 << 2)
#define PT_PADDR_MASK    ((uint64_t)0x0000FFFFFFFFF000)

#define PT_TABLEES   (PTE_VALID | PTE_TABLE)

#define PT_IS_TABLE(x) (((x) & (PTE_VALID | PTE_TABLE)) == (PTE_VALID | PTE_TABLE))
#define PT_IS_LARGE(x) (((x) & (PTE_VALID | PTE_TABLE)) == PTE_VALID)
#define PT_TO_VMMES(x) (pt_to_vmmEs_internal(x))

#define pte_new(addr, flags)    ((pt_entry_t)(addr) | (flags))
#define PTE_GET_ADDR(pte)           ((pte) & PT_PADDR_MASK)
#define PTE_GETES(VALUE) PT_TO_VMMES(VALUE)

static uint64_t pt_to_vmmEs_internal(pt_entry_t entry) {
    uint64_t flags = 0;

    if (!(entry & PTE_READONLY))
        flags |= VMME_WRITE;
    if (entry & PTE_XN)
        flags |= VMME_NOEXEC;
    if (entry & PTE_FB)
        flags |= VMME_FB;

    return flags;
}
#elif defined (__riscv)

#define PTE_VALID       ((uint64_t)1 << 0)
#define PTE_READ        ((uint64_t)1 << 1)
#define PTE_WRITE       ((uint64_t)1 << 2)
#define PTE_EXEC        ((uint64_t)1 << 3)
#define PTE_USER        ((uint64_t)1 << 4)
#define PTE_ACCESSED    ((uint64_t)1 << 6)
#define PTE_DIRTY       ((uint64_t)1 << 7)
#define PTE_PBMT_NC     ((uint64_t)1 << 62)
#define PT_PADDR_MASK       ((uint64_t)0x003ffffffffffc00)

#define PTE_RWX         (PTE_READ | PTE_WRITE | PTE_EXEC)

#define PT_TABLEES      PTE_VALID
#define PT_IS_TABLE(x)      (((x) & (PTE_VALID | PTE_RWX)) == PTE_VALID)
#define PT_IS_LARGE(x)      (((x) & (PTE_VALID | PTE_RWX)) > PTE_VALID)
#define PT_TO_VMMES(x)  (pt_to_vmmEs_internal(x))

#define pte_new(addr, flags)    (((pt_entry_t)(addr) >> 2) | (flags))
#define PTE_GET_ADDR(pte)           (((pte) & PT_PADDR_MASK) << 2)
#define PTE_GETES(VALUE) PT_TO_VMMES(VALUE)

static uint64_t pt_to_vmmEs_internal(pt_entry_t entry) {
    uint64_t flags = 0;

    if (entry & PTE_WRITE)
        flags |= VMME_WRITE;
    if (!(entry & PTE_EXEC))
        flags |= VMME_NOEXEC;
    if (entry & PTE_PBMT_NC)
        flags |= VMME_FB;

    return flags;
}

#endif


typedef struct vma_region {
    uptr vaddr;
    uptr end;

    u64 pages;
    u64 flags;

    uptr paddr;

    u64 ref_count;

    struct vma_region* next;
    struct vma_region* prev;
} vma_region;

typedef struct pagemap{
    uptr* top_lvl;
    vma_region* vma_head;
} pagemap;

extern pagemap* vmm_kernel_pm;

extern symbol text_start_ld;
extern symbol text_end_ld;

extern symbol rodata_start_ld;
extern symbol rodata_end_ld;

extern symbol data_start_ld;
extern symbol data_end_ld;



namespace VMM{
    void Init();
    vma_region* CreateRegion(pagemap* pm, uptr vaddr, uptr paddr, u64 pages, u64 flags);
    vma_region* GetRegion(pagemap* pm, uptr vaddr);
    vma_region* FindRange(pagemap* pm, uptr vaddr);
    void DeleteRegion(vma_region* region);
    uptr* GetNextlvl(uptr* lvl, uptr entry, u64 flags, bool alloc);
    pagemap* NewPM();
    void DestroyPM(pagemap* pm);
    void SwitchPM(pagemap* pm);
    void SwitchPMNocpu(pagemap* pm);
    void Map(pagemap* pm, uptr vaddr, uptr paddr, u64 flags);
    void MapUser(pagemap* pm, uptr vaddr, uptr paddr, u64 flags);
    void MapRange(pagemap* pm, uptr vaddr, uptr paddr, u64 pages, u64 flags);
    void MapUserRange(pagemap* pm, uptr vaddr, uptr paddr, u64 pages, u64 flags);
    void UnmapRange(pagemap* pm, uptr vaddr, u64 pages);
    void* Alloc(pagemap* pm, u64 pages, u64 flags);
    void Free(pagemap* pm, void* ptr, u64 pages);
    uptr GetRegionPAddr(pagemap* pm, uptr ptr);
    uptr GetPage(pagemap* pm, uptr vaddr);
    bool HandlePF(registers* r);
    pagemap* Clone(pagemap* pm);
}

#define vmm_init VMM::Init
#define vmm_create_region VMM::CreateRegion
#define vmm_get_region VMM::GetRegion
#define vmm_find_range VMM::FindRange
#define vmm_delete_region VMM::DeleteRegion
#define vmm_get_next_lvl VMM::GetNextlvl
#define vmm_new_pm VMM::NewPM
#define vmm_destroy_pm VMM::DestroyPM
#define vmm_switch_pm VMM::SwitchPM
#define vmm_switch_pm_nocpu VMM::SwitchPMNocpu
#define vmm_map VMM::Map
#define vmm_unmap VMM::Unmap
#define vmm_map_user VMM::MapUser
#define vmm_map_range VMM::MapRange
#define vmm_map_user_range VMM::MapUserRange
#define vmm_unmap_range VMM::UnmapRange
#define vmm_alloc VMM::Alloc
#define vmm_free VMM::Free
#define vmm_get_region_paddr VMM::GetRegionPAddr
#define vmm_get_page VMM::GetPage
#define vmm_handle_pf VMM::HandlePF
#define vmm_clone VMM::Clone
#define vmm_invlpg_range(vaddr,pages) VMM::INVLPGRange(vaddr,pages)
#define vmm_insert_after VMM::InsertAfter

#endif