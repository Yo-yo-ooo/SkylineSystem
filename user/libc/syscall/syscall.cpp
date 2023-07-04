#include <stdint.h>

#define _R1 "D"
#define _R2 "S"
#define _R3 "d"
#define _R4 "rcx"
#define _R5 "r8"
#define _R6 "r9"
#define _SYSCALL "int $0x80"

uintptr_t syscall(uintptr_t __n, uintptr_t __1, uintptr_t __2, uintptr_t __3, uintptr_t __4, uintptr_t __5, uintptr_t __6) {
    uintptr_t __result;
    __asm__ __volatile__
        ("movq %0, %%" _R4 "\n"
         "movq %1, %%" _R5 "\n"
         "movq %2, %%" _R6 "\n"
         _SYSCALL "\n"
         : "=a"(__result)
         : "a"(__n), _R1(__1), _R2(__2), _R3(__3), "r"(__4), "r"(__5), "r"(__6)
         : "memory", _R4, _R5, _R6);
    return __result;
}

#define syscall6(n,a,b,c,d,e,f) syscall(n,a,b,c,d,e,f)
#define syscall5(n,a,b,c,d,e) syscall(n,a,b,c,d,e,0)
#define syscall4(n,a,b,c,d) syscall(n,a,b,c,d,0,0)
#define syscall3(n,a,b,c) syscall(n,a,b,c,0,0,0)
#define syscall2(n,a,b) syscall(n,a,b,0,0,0,0)
#define syscall1(n,a) syscall(n,a,0,0,0,0,0)