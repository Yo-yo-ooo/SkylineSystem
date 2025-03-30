#include <mem/heap.h>
#include <mem/pmm.h>

#include <arch/x86_64/smp/smp.h>

heap* kernel_heap;

;
namespace Heap{

void Init() {
    kernel_heap = Create(vmm_kernel_pm);
}

heap* Create(pagemap* pm) {
    heap* h = (heap*)HIGHER_HALF(PMM::Alloc(1));
    h->block_head = (heap_block*)HIGHER_HALF(PMM::Alloc(1));
    h->block_head->magic = HEAP_MAGIC;
    h->block_head->next = h->block_head->prev = h->block_head;
    h->block_head->size = 0;
    h->block_head->state = 1;
    h->pm = pm;
    return h;
}

void* Alloc(heap* h, u64 size) {
    lock(&h->hl);
    u64 pages = DIV_ROUND_UP(sizeof(heap_block) + size, PAGE_SIZE);
    u8* buf = (h == kernel_heap ?
                HIGHER_HALF(PMM::Alloc(pages)) :
                vmm_alloc(h->pm, pages, PTE_PRESENT | PTE_WRITABLE | PTE_USER));
    heap_block* block = (heap_block*)buf;
    block->magic = HEAP_MAGIC;
    block->size = size;
    block->state = 1;
    block->prev = h->block_head->prev;
    block->next = h->block_head;
    h->block_head->prev->next = block;
    h->block_head->prev = block;
    unlock(&h->hl);
    return buf + sizeof(heap_block);
}

void Free(heap* h, void* ptr) {
    lock(&h->hl);
    heap_block* block = (heap_block*)(ptr - sizeof(heap_block));
    if (block->magic != HEAP_MAGIC) {
        kprintf("Heap::Free(): Invalid magic at pointer %lx.\n", ptr);
        unlock(&h->hl);
        return;
    }
    block->prev->next = block->next;
    block->next->prev = block->prev;
    u64 pages = DIV_ROUND_UP(sizeof(heap_block) + block->size, PAGE_SIZE);
    u8* buf = (u8*)(ptr - sizeof(heap_block));
    if (h == kernel_heap) {
        buf = PHYSICAL(buf);
        PMM::Free(buf, pages);
    } else {
        VMM::Free(this_cpu()->pm, buf, pages);
    }
    unlock(&h->hl);
}

void* Realloc(heap* h, void* ptr, u64 size) {
    heap_block* block = (heap_block*)(ptr - sizeof(heap_block));
    if (block->magic != HEAP_MAGIC) {
        kprintf("Heap::Realloc(): Invalid magic at pointer %lx.\n", ptr);
        return NULL;
    }
    void* new_ptr = Heap::Alloc(h, size);
    if (!new_ptr)
        return NULL;
    __memcpy(new_ptr, ptr, block->size);
    Heap::Free(h, ptr);
    return new_ptr;
}

uptr GetAllocationPAddr(heap* h, uptr ptr) {
    heap_block* block = (heap_block*)(ptr - sizeof(heap_block));
    if (block->magic != HEAP_MAGIC)
        return 0;
    return vmm_get_region_paddr(h->pm, (ptr - sizeof(heap_block)));
}

void Clone(heap* h, heap* clone) {
    // Here we set the clone block head to the original block head,
    // which contains the linked list of allocations!

    clone->block_head = h->block_head;
}

void Destroy(heap* h) {
    for (heap_block* block = h->block_head->next; block != h->block_head; block = block->next) {
        u8* buf = (u8*)block;
        vma_region* region = vmm_find_range(h->pm, (uptr)buf);
        if (region->ref_count >= 1) {
            vmm_free(h->pm, (void*)region->vaddr, region->pages);
        }
    }
}

}