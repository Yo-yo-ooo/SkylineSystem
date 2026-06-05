#include <stdint.h>
#include <stddef.h>
#include <base/arch/x86_64/syscalln.h>
#include <base/arch/x86_64/syscall.h>

long syscall(long number,long arg1,long arg2, long arg3,long arg4,long arg5,long arg6){
    register long rax asm("rax") = number;
    register long rdi asm("rdi") = arg1;
    register long rsi asm("rsi") = arg2;
    register long rdx asm("rdx") = arg3;
    register long r10 asm("r10") = arg4;
    register long r8 asm("r8") = arg5;
    register long r9 asm("r9") = arg6;
    asm volatile("syscall" 
        : "+r"(rax) 
        : "r"(rdi),"r"(rsi),"r"(rdx),
        "r"(r10),"r"(r8),"r"(r9)
        : "rcx","r11","memory"
    );
    return rax;
}

uint64_t mmap(uint64_t addr,uint64_t length, uint64_t mode,
uint64_t flags,uint64_t offset){
    return syscall(SYSCALL_MMAP,addr,length,mode,flags,offset,0);    
}


uint64_t munmap(uint64_t addr,uint64_t length){
    syscall(SYSCALL_MUNMAP,addr,length,0,0,0,0);    
    return 0;
}

