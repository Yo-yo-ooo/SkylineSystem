#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/kio.h>
#include <arch/x86_64/asm/prctl.h>

// IN LIBC This SYSCALL EXPRESS: uint64_t sys_dev_read(DevType dt,uint64_t DevIDX,...)
uint64_t sys_dev_read(uint64_t DevType,uint64_t DevIDX,\
uint64_t buffer,uint64_t count,uint64_t offset,uint64_t ign_0){
    IGNORE_VALUE(ign_0);
    return Dev::DeviceRead(DevType,DevIDX,offset,(void*)buffer,count);
}

uint64_t sys_dev_write(uint64_t DevType,uint64_t DevIDX,\
uint64_t buffer,uint64_t count,uint64_t offset,uint64_t ign_0){
    IGNORE_VALUE(ign_0);
    return Dev::DeviceWrite(DevType,DevIDX,offset,(void*)buffer,count);
}

uint64_t sys_dev_mmap(uint64_t DevType,uint64_t DevIDX,
uint64_t length,uint64_t prot,uint64_t flags,uint64_t offset){
    
}