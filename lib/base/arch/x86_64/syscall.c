//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

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
uint64_t sys_fsize(int32_t fd)
{return syscall(SYSCALL_FSIZE,fd,0,0,0,0,0);}
void sys_exit(uint64_t status)
{(void)status;syscall(SYSCALL_EXIT,0,0,0,0,0,0);}
uint64_t sys_pmmapSHARE(
    uint64_t dst_pid, uint64_t dst_addr, uint64_t length,
    uint64_t flags,   uint64_t src_pid, uint64_t src_addr
) {return syscall(SYSCALL_PMMAP,dst_pid,dst_addr,length,flags,src_pid,src_addr);}
uint64_t sched_yield(){return syscall(SYSCALL_YIELD,0,0,0,0,0,0);}
uint64_t sys_load(uint64_t pathname, uint64_t argv, uint64_t envp)
{return syscall(SYSCALL_LOAD,pathname,argv,envp,0,0,0);}
uint64_t sys_launch(uint64_t pid)
{return syscall(SYSCALL_LAUNCH,pid,0,0,0,0,0);}
uint64_t sys_getpid(){return syscall(SYSCALL_GETPID,0,0,0,0,0,0);}
uint64_t sys_gettid(){return syscall(SYSCALL_GETTID,0,0,0,0,0,0);}
uint64_t sys_thread_launch(uint64_t entry, uint64_t hint)
{return syscall(SYSCALL_THREAD_LAUNCH,entry,hint,0,0,0,0);}
uint64_t sys_getsysinfo()
{return syscall(SYSCALL_GET_SYSINFO,0,0,0,0,0,0);}