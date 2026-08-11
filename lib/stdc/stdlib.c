#include <stdlib.h>

#ifdef __x86_64__
#include <base/arch/x86_64/syscall.h>
#endif

void _Exit(){
#ifdef __x86_64__
    sys_exit(0);
#endif
    __builtin_unreachable();
}