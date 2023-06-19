#include <stddef.h>

void* _Ymalloc(size_t size, const char* func, const char* file, int line);
void _Yfree(void* address,const char* func, const char* file, int line);