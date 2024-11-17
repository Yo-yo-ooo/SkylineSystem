#pragma once

#include "../klib/klib.h"
#include "vmm.h"

#define HEAP_MAGIC 0xdeadbeef

typedef struct heap_block {
    struct heap_block* next;
    struct heap_block* prev;
    u8 state;
    u32 magic;
    u64 size;
} heap_block;

typedef struct {
    atomic_lock hl;
    heap_block* block_head;
    pagemap* pm;
} heap;

namespace Heap {
    //Initialize the kernel heap not the user heap
    void Init();

    heap* Create(pagemap* pm);
    void Destroy(heap* h);
    void Clone(heap* h, heap* clone);

    uptr GetAllocationPAddr(heap* h, uptr ptr);

    void* Alloc(heap* h, u64 size);
    void Free(heap* h, void* ptr);
    void* Realloc(heap* h,void* ptr, u64 size);
}

extern heap* kernel_heap;

void* kmalloc(u64 size);
void kfree(void* ptr);
void* krealloc(void* ptr, u64 size);