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

uint64_t sys_mmap(uint64_t addr,uint64_t length, uint64_t mode,
uint64_t flags,uint64_t offset){
    return syscall(SYSCALL_MMAP,addr,length,mode,flags,offset,0);    
}


uint64_t sys_munmap(uint64_t addr,uint64_t length){
    syscall(SYSCALL_MUNMAP,addr,length,0,0,0,0);    
    return 0;
}

uint64_t sys_fread(int32_t fd_idx, uint64_t buf, uint64_t count)
{return syscall(SYSCALL_FREAD,fd_idx,buf,count,0,0,0);}
uint64_t sys_fwrite(int32_t fd_idx, uint64_t buf, uint64_t count)
{return syscall(SYSCALL_FWRITE,fd_idx,buf,count,0,0,0);}
uint64_t sys_flseek(int32_t fd_idx, uint64_t offset, uint64_t whence)
{return syscall(SYSCALL_FLSEEK,fd_idx,offset,whence,0,0,0);}
uint64_t sys_fopen(uint64_t path, uint64_t flags)
{return syscall(SYSCALL_FOPEN,path,flags,0,0,0,0);}
uint64_t sys_fclose(int32_t fd)
{return syscall(SYSCALL_FCLOSE,fd,0,0,0,0,0);}