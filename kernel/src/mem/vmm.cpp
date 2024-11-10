#include <limine.h>
#include "vmm.h"   
#include "pmm.h"

__attribute__((used, section(".requests")))
static volatile struct limine_kernel_address_request kernel_address_request = {
  .id = LIMINE_KERNEL_ADDRESS_REQUEST,
  .revision = 0
};

pagemap* vmm_kernel_pm = NULL;

void vmm_init() {
    vmm_kernel_pm = (pagemap*)HIGHER_HALF(pmm_alloc(1));
    memset(vmm_kernel_pm, 0, PAGE_SIZE);

    vmm_kernel_pm->top_lvl = (uptr*)HIGHER_HALF(pmm_alloc(1));
    memset(vmm_kernel_pm->top_lvl, 0, PAGE_SIZE);

    vmm_kernel_pm->vma_head = (vma_region*)HIGHER_HALF(pmm_alloc(1));
    memset(vmm_kernel_pm->vma_head, 0, PAGE_SIZE);

    vmm_kernel_pm->vma_head->next = vmm_kernel_pm->vma_head;
    vmm_kernel_pm->vma_head->prev = vmm_kernel_pm->vma_head;

    uptr phys_base = kernel_address_request.response->physical_base;
    uptr virt_base = kernel_address_request.response->virtual_base;

    uptr text_start = ALIGN_DOWN((uptr)text_start_ld, PAGE_SIZE);
    uptr text_end = ALIGN_UP((uptr)text_end_ld, PAGE_SIZE);
    uptr rodata_start = ALIGN_DOWN((uptr)rodata_start_ld, PAGE_SIZE);
    uptr rodata_end = ALIGN_UP((uptr)rodata_end_ld, PAGE_SIZE);
    uptr data_start = ALIGN_DOWN((uptr)data_start_ld, PAGE_SIZE);
    uptr data_end = ALIGN_UP((uptr)data_end_ld, PAGE_SIZE);

    for (uptr text = text_start; text < text_end; text += PAGE_SIZE)
        vmm_map(vmm_kernel_pm, text, text - virt_base + phys_base, PTE_PRESENT);
    for (uptr rodata = rodata_start; rodata < rodata_end; rodata += PAGE_SIZE)
        vmm_map(vmm_kernel_pm, rodata, rodata - virt_base + phys_base, PTE_PRESENT | PTE_NX);
    for (uptr data = data_start; data < data_end; data += PAGE_SIZE)
        vmm_map(vmm_kernel_pm, data, data - virt_base + phys_base, PTE_PRESENT | PTE_WRITABLE | PTE_NX);
    for (uptr gb4 = 0; gb4 < 0x100000000; gb4 += PAGE_SIZE) {
        vmm_map(vmm_kernel_pm, gb4, gb4, PTE_PRESENT | PTE_WRITABLE);
        vmm_map(vmm_kernel_pm, (uptr)HIGHER_HALF(gb4), gb4, PTE_PRESENT | PTE_WRITABLE);
    }

    vmm_switch_pm_nocpu(vmm_kernel_pm);
    kprintf("[INFO] vmm_init(): VMM Initialised. Kernel's page map located at %lx.\n", (u64)vmm_kernel_pm);
}

vma_region* vmm_create_region(pagemap* pm, uptr vaddr, uptr paddr, u64 pages, u64 flags) {
    vma_region* region = (vma_region*)HIGHER_HALF(pmm_alloc(1));
    region->vaddr = vaddr;
    region->end = vaddr + (pages * PAGE_SIZE);
    region->paddr = paddr;
    region->pages = pages;
    region->flags = flags;

    region->ref_count = 0;

    region->prev = pm->vma_head->prev;
    region->next = pm->vma_head;

    pm->vma_head->prev->next = region;
    pm->vma_head->prev = region;

    return region;
}

vma_region* vmm_insert_after(vma_region* prev, uptr vaddr, uptr paddr, u64 pages, u64 flags) {
    vma_region* region = (vma_region*)HIGHER_HALF(pmm_alloc(1));
    region->vaddr = vaddr;
    region->end = vaddr + (pages * PAGE_SIZE);
    region->paddr = paddr;
    region->pages = pages;
    region->flags = flags;
    region->ref_count = 0;

    region->prev = prev;
    region->next = prev->next;

    prev->next->prev = region;
    prev->next = region;

    return region;
}

void vmm_delete_region(vma_region* region) {
    region->prev->next = region->prev;
    region->next->prev = region->prev;

    pmm_free(PHYSICAL(region), 1);
}

vma_region* vmm_get_region(pagemap* pm, uptr vaddr) {
    vma_region* region = pm->vma_head->next;
    for (; region != pm->vma_head; region = region->next)
        if (region->vaddr == vaddr)
        return region;
    return NULL;
}

vma_region* vmm_find_range(pagemap* pm, uptr vaddr) {
    vma_region* region = pm->vma_head->next;
    for (; region != pm->vma_head; region = region->next)
        if (region->vaddr <= vaddr && region->end >= vaddr)
        return region;
    return NULL;
}

uptr* vmm_get_next_lvl(uptr* lvl, uptr entry, u64 flags, bool alloc) {
    if (lvl[entry] & PTE_PRESENT)
        return (uptr*)HIGHER_HALF(PTE_GET_ADDR(lvl[entry]));
    if (alloc) {
        uptr* pml = (uptr*)HIGHER_HALF(pmm_alloc(1));
        memset(pml, 0, PAGE_SIZE);
        lvl[entry] = (uptr)PHYSICAL(pml) | flags;
        return pml;
    }
    return NULL;
}

pagemap* vmm_new_pm() {
    pagemap* pm = (pagemap*)HIGHER_HALF(pmm_alloc(1));
    memset(pm, 0, PAGE_SIZE);

    pm->top_lvl = (uptr*)HIGHER_HALF(pmm_alloc(1));
    memset(pm->top_lvl, 0, PAGE_SIZE);

    pm->vma_head = (vma_region*)HIGHER_HALF(pmm_alloc(1));
    memset(pm->vma_head, 0, PAGE_SIZE);

    pm->vma_head->next = pm->vma_head;
    pm->vma_head->prev = pm->vma_head;

    for (usize i = 256; i < 512; i++)
        pm->top_lvl[i] = vmm_kernel_pm->top_lvl[i];
    return pm;
}

void vmm_destroy_pm(pagemap* pm) {
    vma_region* next;
    for (vma_region* region = pm->vma_head->next; region != pm->vma_head;) {
        next = region->next;
        pmm_free(PHYSICAL(region), 1);
        region = next;
    }
    pmm_free(PHYSICAL(pm->vma_head), 1);
    pmm_free(PHYSICAL(pm->top_lvl), 1);
    pmm_free(PHYSICAL(pm), 1);
}

void vmm_switch_pm_nocpu(pagemap* pm) {
#ifdef __x86_64__
    __asm__ volatile ("mov %0, %%cr3" : : "r"((u64)PHYSICAL(pm->top_lvl)) : "memory");
#elif defined(__aarch64__)
    write_ttbr_el1(1,(u64)PHYSICAL(pm->top_lvl));
#endif
}

void vmm_switch_pm(pagemap* pm) {
    //__asm__ volatile ("mov %0, %%cr3" : : "r"((u64)PHYSICAL(pm->top_lvl)) : "memory");
    //this_cpu()->pm = pm;
}

void vmm_map(pagemap* pm, uptr vaddr, uptr paddr, u64 flags) {
    uptr pml1_entry = (vaddr >> 12) & 0x1ff;
    uptr pml2_entry = (vaddr >> 21) & 0x1ff;
    uptr pml3_entry = (vaddr >> 30) & 0x1ff;
    uptr pml4_entry = (vaddr >> 39) & 0x1ff;

    uptr* pml3 = vmm_get_next_lvl(pm->top_lvl, pml4_entry, PTE_PRESENT | PTE_WRITABLE, true);       // pml4[pml4Entry] = pml3
    uptr* pml2 = vmm_get_next_lvl(pml3, pml3_entry, PTE_PRESENT | PTE_WRITABLE, true);     // pml3[pml3Entry] = pml2
    uptr* pml1 = vmm_get_next_lvl(pml2, pml2_entry, PTE_PRESENT | PTE_WRITABLE, true);     // pml2[pml2Entry] = pml1

    pml1[pml1_entry] = paddr | flags;
}

void vmm_map_user(pagemap* pm, uptr vaddr, uptr paddr, u64 flags) {
    uptr pml1_entry = (vaddr >> 12) & 0x1ff;
    uptr pml2_entry = (vaddr >> 21) & 0x1ff;
    uptr pml3_entry = (vaddr >> 30) & 0x1ff;
    uptr pml4_entry = (vaddr >> 39) & 0x1ff;

    uptr* pml3 = vmm_get_next_lvl(pm->top_lvl, pml4_entry, flags, true);
    uptr* pml2 = vmm_get_next_lvl(pml3, pml3_entry, flags, true);
    uptr* pml1 = vmm_get_next_lvl(pml2, pml2_entry, flags, true);

    pml1[pml1_entry] = paddr | flags;
}

void vmm_unmap(pagemap* pm, uptr vaddr) {
    uptr pml1_entry = (vaddr >> 12) & 0x1ff;
    uptr pml2_entry = (vaddr >> 21) & 0x1ff;
    uptr pml3_entry = (vaddr >> 30) & 0x1ff;
    uptr pml4_entry = (vaddr >> 39) & 0x1ff;

    uptr* pml3 = vmm_get_next_lvl(pm->top_lvl, pml4_entry, 0, false);
    if (pml3 == NULL) return;
    uptr* pml2 = vmm_get_next_lvl(pml3, pml3_entry, 0, false);
    if (pml2 == NULL) return;
    uptr* pml1 = vmm_get_next_lvl(pml2, pml2_entry, 0, false);
    if (pml1 == NULL) return;
    pml1[pml1_entry] = 0;
    __asm__ volatile ("invlpg (%0)" : : "b"(vaddr) : "memory");
}

uptr vmm_get_page(pagemap* pm, uptr vaddr) {
    uptr pml1_entry = (vaddr >> 12) & 0x1ff;
    uptr pml2_entry = (vaddr >> 21) & 0x1ff;
    uptr pml3_entry = (vaddr >> 30) & 0x1ff;
    uptr pml4_entry = (vaddr >> 39) & 0x1ff;

    uptr* pml3 = vmm_get_next_lvl(pm->top_lvl, pml4_entry, 0, false);
    if (!pml3) return 0;
    uptr* pml2 = vmm_get_next_lvl(pml3, pml3_entry, 0, false);
    if (!pml2) return 0;
    uptr* pml1 = vmm_get_next_lvl(pml2, pml2_entry, 0, false);
    if (!pml1) return 0;

    return pml1[pml1_entry];
}

uptr vmm_get_pml2(pagemap* pm, uptr vaddr) {
    uptr pml2_entry = (vaddr >> 21) & 0x1ff;
    uptr pml3_entry = (vaddr >> 30) & 0x1ff;
    uptr pml4_entry = (vaddr >> 39) & 0x1ff;

    uptr* pml3 = vmm_get_next_lvl(pm->top_lvl, pml4_entry, 0, false);
    if (!pml3) return 0;
    uptr* pml2 = vmm_get_next_lvl(pml3, pml3_entry, 0, false);
    if (!pml2) return 0;
    return pml2[pml2_entry];
}

void vmm_map_range(pagemap* pm, uptr vaddr, uptr paddr, u64 pages, u64 flags) {
    for (u64 i = 0; i < pages; i++)
        vmm_map(pm, vaddr + (i * PAGE_SIZE), paddr + (i * PAGE_SIZE), flags);
}

void vmm_map_user_range(pagemap* pm, uptr vaddr, uptr paddr, u64 pages, u64 flags) {
    for (u64 i = 0; i < pages; i++)
        vmm_map_user(pm, vaddr + (i * PAGE_SIZE), paddr + (i * PAGE_SIZE), flags);
}

void vmm_unmap_range(pagemap* pm, uptr vaddr, u64 pages) {
    for (u64 i = 0; i < pages; i++)
        vmm_unmap(pm, vaddr + (i * PAGE_SIZE));
}

void* vmm_alloc(pagemap* pm, u64 pages, u64 flags) {
    void* pg = pmm_alloc(pages);
    if (!pg) return NULL;
    // In case we didn't find a hole, create a new region
    uptr vaddr = pm->vma_head->prev->end + PAGE_SIZE;
    bool found = false;
    vma_region* region = pm->vma_head->next;
    for (; region != pm->vma_head; region = region->next) {
        if (region->end >= region->next->vaddr)
        continue;
        // We found a hole, now check it's size
        if (region->next->vaddr - region->end >= ((pages + 1) * PAGE_SIZE)) {
        vaddr = region->end + PAGE_SIZE;
        region = vmm_insert_after(region, vaddr, (uptr)pg, pages, flags);
        found = true;
        break;
        }
    }
    if (!found) region = vmm_create_region(pm, vaddr, (uptr)pg, pages, flags);
    vmm_map_user_range(pm, vaddr, (uptr)pg, pages, flags);
    region->ref_count++;
    return (void*)vaddr;
}

void vmm_free(pagemap* pm, void* ptr, u64 pages) {
    if (!ptr) return;
    vma_region* region = vmm_get_region(pm, (uptr)ptr);
    if (!region)
        return;
    pmm_free((void*)region->paddr, pages);
    vmm_unmap_range(pm, region->vaddr, pages);
    vmm_delete_region(region);
}

uptr vmm_get_region_paddr(pagemap* pm, uptr ptr) {
    vma_region* region = vmm_get_region(pm, ptr);
    if (!region)
        return 0;
    return region->paddr;
}

void vmm_invlpg_range(uptr vaddr, u64 pages) {
    for (u64 i = 0; i < pages; i++)
        __asm__ volatile ("invlpg (%0)" : : "r"(vaddr + (i * PAGE_SIZE)));
}

bool vmm_handle_pf(registers* r) {
    bool halt = false;
    /*
    if (this_cpu()->pm == vmm_kernel_pm) {
        printf("cpu%lu: Page fault. Died.\n", this_cpu()->lapic_id);
        kprintf("cpu%lu: Page fault. Died.\n", this_cpu()->lapic_id);
        halt = true;
    } else
    */
        halt = false;
    u64 cr2;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    //pagemap* pm = this_thread()->pm;
    pagemap* pm = {0};
    u64 vaddr = ALIGN_DOWN(cr2, PAGE_SIZE);
    uptr page = vmm_get_page(pm, vaddr);
    uptr paddr = page & ~0xFFF;
    if (!halt) {
        // Check if the page is a CoW
        if (!(page & PTE_PRESENT))
        goto nocow;
        if (page & PTE_WRITABLE)
        goto nocow;

        vma_region* region = vmm_find_range(pm, cr2);
        if (!region)
        goto nocow;

        void* newpage = pmm_alloc(region->pages);
        memcpy(HIGHER_HALF(newpage), HIGHER_HALF(region->paddr), region->pages * PAGE_SIZE);

        // It is CoW now we just need to re-map it.
        vmm_map_range(pm, region->vaddr, (uptr)newpage, region->pages, region->flags);
        region->paddr = (uptr)newpage;
        region->ref_count++;

        vmm_invlpg_range(region->vaddr, region->pages);

        return false;
}

nocow:
    //kprintf("Segmentation fault on proc %lu\n", this_proc()->pid);

    kprintf("Vaddr: %lx Paddr: %lx PM: %lx Page flags: 0x%lx\n", vaddr, paddr, pm, page & 0xFFF);
    kprintf("RIP: 0x%lx RSP: 0x%lx CR2: 0x%lx err: %lx\n", r->rip, r->rsp, cr2, r->err_code);
    //if (!halt)
        //sig_raise(SIGSEGV);
    return halt;
}

pagemap* vmm_clone(pagemap* pm) {
    pagemap* clone = vmm_new_pm();

    // Create a new region for the clone.

    for (vma_region* region = pm->vma_head->next; region != pm->vma_head; region = region->next) {
        // Set page as read only.
        vmm_map_range(pm, region->vaddr, region->paddr, region->pages, region->flags & ~PTE_WRITABLE);

        // Map the directories with the right flags
        vmm_map_user_range(clone, region->vaddr, region->paddr, region->pages, region->flags);
        // Map the page with the right flags
        vmm_map_range(clone, region->vaddr, region->paddr, region->pages, region->flags & ~PTE_WRITABLE);

        vmm_invlpg_range(region->vaddr, region->pages);

        // Create a new region in clone vma
        vmm_create_region(clone, region->vaddr, region->paddr, region->pages, region->flags)->ref_count = 0;
        region->ref_count = 0;
    }

  return clone;
}
