#include "new2.h"
#include "heap.h"

void* _Ymalloc(size_t size, const char* func, const char* file, int line)
{
    return _Xmalloc(size, func, file, line);
}

void _Yfree(void* address,const char* func, const char* file, int line){
    _Xfree(address,func,file,line);
}