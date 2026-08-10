#include <stdint.h>


static __thread int __errno = 0;

int *_errno(){
    return &__errno;    
}