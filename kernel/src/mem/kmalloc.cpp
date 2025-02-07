#include <mem/heap.h>


void* kmalloc(u64 size) {
    return Heap::Alloc(kernel_heap, size);
}

void kfree(void* ptr) {
    Heap::Free(kernel_heap, ptr);
}

void* krealloc(void* ptr, u64 size) {
    return Heap::Realloc(kernel_heap, ptr, size);
}

#define align4(x) (((((x)-1)>>2)<<2)+4)
void *kcalloc(size_t numitems, size_t size)
{
    size_t *knew;
    size_t s, i;
    knew = kmalloc(numitems * size);
    if(knew)
    {
        s = align4(numitems * size) >> 2;
        for(i = 0; i < s; ++i)
            knew[i] = 0;
    }
    return knew;
}
