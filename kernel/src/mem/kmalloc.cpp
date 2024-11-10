#include "heap.h"


void* kmalloc(u64 size) {
    return Heap::Alloc(kernel_heap, size);
}

void kfree(void* ptr) {
    Heap::Free(kernel_heap, ptr);
}

void* krealloc(void* ptr, u64 size) {
    return Heap::Realloc(kernel_heap, ptr, size);
}