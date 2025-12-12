#pragma once
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include <libfg.h>

#define syscall6(num, a,b, c, d, e, f,retnum) ({            \
    register uint64_t argd asm("r10") = (uint64_t)(d);      \
    register uint64_t argd asm("r8") = (uint64_t)(e);       \
    register uint64_t argd asm("r9") = (uint64_t)(f);       \
    asm volatile("syscall"                                  \
        :"=a"(retnum) : "0"(num),"D"(a),"S"(b),"d"(c),      \
        "r"(argd),"r"(arge),"r"(argf)                       \
        : "rcx", "r11","memory");                                        \
})


#define syscall5(num, a,b, c, d, e,retnum) ({               \
    register uint64_t argd asm("r10") = (uint64_t)(d);      \
    register uint64_t argd asm("r8") = (uint64_t)(e);       \
    asm volatile("syscall"                                  \
        :"=a"(retnum) : "0"(num),"D"(a),"S"(b),"d"(c),      \
        "r"(argd),"r"(arge)                                 \
        : "rcx", "r11","memory");                                        \
})

#define syscall4(num, a,b, c, d,retnum) ({                  \
    register uint64_t argd asm("r10") = (uint64_t)(d);      \
    asm volatile("syscall"                                  \
        :"=a"(retnum) : "0"(num),"D"(a),"S"(b),"d"(c),      \
        "r"(argd)                                           \
        : "rcx", "r11","memory");                                        \
})

#define syscall3(num, a,b, c,retnum) ({                     \
    asm volatile("syscall"                                  \
        :"=a"(retnum) : "0"(num),"D"(a),"S"(b),"d"(c)       \
        : "rcx", "r11","memory");                                        \
})

#define syscall2(num, a,b,retnum) ({                        \
    asm volatile("syscall"                                  \
        :"=a"(retnum) : "0"(num),"D"(a),"S"(b)              \
        : "rcx", "r11","memory");                                        \
})

#define syscall1(num, a,retnum) ({                          \
    asm volatile("syscall"                                  \
        :"=a"(retnum) : "0"(num),"D"(a)                     \
        : "rcx", "r11","memory");                                        \
})

#define syscall0(num,retnum) ({                             \
    asm volatile("syscall"                                  \
        :"=a"(retnum) : "0"(num)                            \
        : "rcx", "r11","memory");                                        \
})

#endif