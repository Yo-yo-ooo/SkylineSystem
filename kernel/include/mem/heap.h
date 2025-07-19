#pragma once

#include <klib/klib.h>
#ifdef __x86_64__
#include <arch/x86_64/vmm/vmm.h>
#endif

#define HEAP_MAGIC 0xdeadbeef

struct vma_region;
typedef struct vma_region vma_region;
struct pagemap;
typedef struct pagemap pagemap;
struct atomic_lock_t;
typedef struct atomic_lock_t atomic_lock_t;
typedef struct heap_block {
    struct heap_block* next;
    struct heap_block* prev;
    u8 state;
    u32 magic;
    u64 size;
} heap_block;

typedef struct {
    atomic_lock_t hl;
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
void *kcalloc(size_t numitems, size_t size);

inline void operator delete(void* p) {kfree(p);}

#include "new.hpp"