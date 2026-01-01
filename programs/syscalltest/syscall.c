#include "syscall.h"

long syscall(long number,long arg1,long arg2, \
long arg3,long arg4,long arg5,long arg6){
    register long rax asm("rax") = number;
    register long rdi asm("rdi") = arg1;
    register long rsi asm("rsi") = arg2;
    register long rdx asm("rdx") = arg3;
    register long r10 asm("r10") = arg4;
    register long r8 asm("r8") = arg5;
    register long r9 asm("r9") = arg6;
    asm volatile("syscall" 
        : "=r"(rax) 
        : "r"(rdi),"r"(rsi),"r"(rdx),
        "r"(r10),"r"(r8),"r"(r9)
        : "rcx","r11","memory"
    );
    return rax;
}