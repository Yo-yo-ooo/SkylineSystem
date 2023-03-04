#include <../../kernel/kernel/src/memory/heap.h>

void* malloc(size_t size){
    _Xmalloc(size,__PRETTY_FUNCTION__,__FILE__,__LINE__);
}

void free(void *ptr){
    _Free(ptr);
}

template <typename T,typename U>
T cast_type(T a,U b) -> decltype(a+b){
    return reinterpret_cast<T>b;
}

