#pragma once

#include <types.h>
#include <mm/pmm.h>
#include <sys/idt.h>

#define PTE_PRESENT (u64)1
#define PTE_WRITABLE (u64)2
#define PTE_USER (u64)4
#define PTE_NX (1ull << 63)

#define PTE_ADDR_MASK 0x000ffffffffff000
#define PTE_GET_ADDR(VALUE) ((VALUE) & PTE_ADDR_MASK)
#define PTE_GET_FLAGS(VALUE) ((VALUE) & ~PTE_ADDR_MASK)

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

typedef struct {
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

void vmm_init();

pagemap* vmm_new_pm();
void vmm_destroy_pm(pagemap* pm);

void vmm_switch_pm_nocpu(pagemap* pm);
void vmm_switch_pm(pagemap* pm);

vma_region* vmm_create_region(pagemap* pm, uptr vaddr, uptr paddr, u64 pages, u64 flags);
void vmm_delete_region(vma_region* region);

void vmm_map(pagemap* pm, uptr vaddr, uptr paddr, u64 flags);
void vmm_map_user(pagemap* pm, uptr vaddr, uptr paddr, u64 flags);
void vmm_unmap(pagemap* pm, uptr vaddr);

uptr vmm_get_page(pagemap* pm, uptr vaddr);

void vmm_map_range(pagemap* pm, uptr vaddr, uptr paddr, u64 pages, u64 flags);
void vmm_map_user_range(pagemap* pm, uptr vaddr, uptr paddr, u64 pages, u64 flags);

void* vmm_alloc(pagemap* pm, u64 pages, u64 flags);
void vmm_free(pagemap* pm, void* ptr, u64 pages);

vma_region* vmm_find_range(pagemap* pm, uptr vaddr);
uptr vmm_get_region_paddr(pagemap* pm, uptr ptr);

bool vmm_handle_pf(registers* r);

pagemap* vmm_clone(pagemap* pm);
