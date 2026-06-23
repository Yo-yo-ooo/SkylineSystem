#include <stdint.h>
#include <stddef.h>
#include <base/arch/x86_64/syscalln.h>
#include <base/arch/x86_64/syscall.h>



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