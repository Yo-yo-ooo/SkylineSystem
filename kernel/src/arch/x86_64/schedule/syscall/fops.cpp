#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <fs/fd.h>

uint64_t sys_read(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    fd_t *fd = Schedule::this_proc()->fd_table[fd_idx];
    if (!fd)
        return -EBADF;
    ext4_fread(&fd->f,(const void*)buf,count,NULL);
    fd->off += count;
    return count;
}

uint64_t sys_write(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
        IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    fd_t *fd = Schedule::this_proc()->fd_table[fd_idx];
    if(fd_idx == 1){
        kinfo();
        for(size_t i = 0;i < count;i++){
            kprintf("%c",*((char*)buf + i));
        }
        kprintf("\n");
        return count;
    }else if(fd_idx == 2){
        return count;
    }else if(fd_idx == 0){
        printf_("[\033[38;2;255;0;0mERR FORM USER %d\033[0m]",Schedule::this_proc());
        for(size_t i = 0;i < count;i++){
            kprintf("%c",*((char*)buf + i));
        }
        kprintf("\n");
        return count;
    }
    if (!fd)
        return -EBADF;
    ext4_fwrite(&fd->f,(const void*)buf,count,NULL);
    fd->off += count;
    return count;
}

int64_t sys_lseek(uint64_t fd_idx, uint64_t offset, uint64_t whence, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2){
    fd_t *fd = Schedule::this_proc()->fd_table[fd_idx];
    if (!fd)
        return -EBADF;
    if(ext4_fseek(&fd->f,fd->off,(uint32_t)whence) == EOK)
        return offset;
    else{return -1;}
}


uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
        IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    proc_t *proc = Schedule::this_proc();

    fd_t *fd = (fd_t*)kmalloc(sizeof(fd));
    fd->flags = (int32_t)flags;
    fd->off = 0;
    fd->path = (char*)path;
    ext4_fopen(&fd->f,path,flags);

    if (!fd) return (uint64_t)((int64_t)-1);
    proc->fd_table[proc->fd_count++] = fd;
    return proc->fd_count - 1;
}
