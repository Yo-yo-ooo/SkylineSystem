#pragma once

#ifndef _VMM_H_
#define _VMM_H_

#include "../../../klib/klib.h"
#include "../interrupt/idt.h"

#define PTE_PRESENT (u64)1
#define PTE_WRITABLE (u64)2
#define PTE_USER (u64)4
#define PTE_NX (1ull << 63)

#define PTE_ADDR_MASK 0x000ffffffffff000
#define PTE_GET_ADDR(VALUE) ((VALUE) & PTE_ADDR_MASK)
#define PTE_GETES(VALUE) ((VALUE) & ~PTE_ADDR_MASK)

struct registers__;
typedef struct registers__ registers;



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
    void Map(uptr vaddr, uptr paddr);
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