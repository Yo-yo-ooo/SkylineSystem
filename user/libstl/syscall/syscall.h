#include <stdint.h>

#define _R1 "D"
#define _R2 "S"
#define _R3 "d"
#define _R4 "rcx"
#define _R5 "r8"
#define _R6 "r9"
#define _SYSCALL "int $0x80"

#define __a uintptr_t

inline uintptr_t syscall6(uintptr_t __n, uintptr_t __1, uintptr_t __2, uintptr_t __3, uintptr_t __4, uintptr_t __5, uintptr_t __6) {
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

inline __a syscall4(__a __n, __a __1, __a __2, __a __3, __a __4) {
    __a __result;
    __asm__ __volatile__
        ("movq %0, %%" _R4 "\n"
         _SYSCALL "\n"
         : "=a"(__result)
         : "a"(__n), _R1(__1), _R2(__2), _R3(__3), "r"(__4)
         : "memory", _R4);
    return __result;
}

inline __a syscall3(__a __n, __a __1, __a __2, __a __3) {
    __a __result;
    __asm__ __volatile__
        ("movq %0, %%" _R4 "\n"
         _SYSCALL "\n"
         : "=a"(__result)
         : "a"(__n), _R1(__1), _R2(__2),"r"(__3)
         : "memory", _R4);
    return __result;
}

inline __a syscall2(__a __n, __a __1, __a __2) {
    __a __result;
    __asm__ __volatile__
        ("movq %0, %%" _R4 "\n"
         _SYSCALL "\n"
         : "=a"(__result)
         : "a"(__n), _R1(__1), _R2(__2)
         : "memory", _R4);
    return __result;
}

inline __a syscall1(__a __n, __a __1) {
    __a __result;
    __asm__ __volatile__
        ("movq %0, %%" _R4 "\n"
         _SYSCALL "\n"
         : "=a"(__result)
         : "a"(__n), _R1(__1)
         : "memory", _R4);
    return __result;
}


inline __a syscall5(__a __n, __a __1, __a __2, __a __3, __a __4, __a __5) {
    __a __result;
    __asm__ __volatile__
        ("movq %0, %%" _R4 "\n"
         "movq %1, %%" _R5 "\n"
         _SYSCALL "\n"
         : "=a"(__result)
         : "a"(__n), _R1(__1), _R2(__2), _R3(__3), "r"(__4), "r"(__5)
         : "memory", _R4, _R5);
    return __result;
}