#include <stdint.h>

char *sbrk(intptr_t increment); 
void *malloc(unsigned int nbytes);
template<typename T>
T Fmalloc(unsigned int size);
void free(void *ap);