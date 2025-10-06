#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <fs/fd.h>

uint64_t sys_read(uint32_t fd_idx, void *buf, size_t count) {
    fd_t *fd = Schedule::this_proc()->fd_table[fd_idx];
    if (!fd)
        return -EBADF;
    ext4_fread(&fd->f,buf,count,NULL);
    fd->off += count;
    return count;
}

uint64_t sys_write(uint32_t fd_idx, void *buf, size_t count) {
    fd_t *fd = Schedule::this_proc()->fd_table[fd_idx];
    if (!fd)
        return -EBADF;
    if(fd_idx == 1){
        Serial::Writelnf("%s",(char*)buf);
        return count;
    }else if(fd_idx == 2){
        return count;
    }else if(fd_idx == 0){
        return -EBADF;
    }
    ext4_fwrite(&fd->f,buf,count,NULL);
    fd->off += count;
    return count;
}

int64_t sys_lseek(uint32_t fd_idx, int64_t offset, int32_t whence){
    fd_t *fd = Schedule::this_proc()->fd_table[fd_idx];
    if (!fd)
        return -EBADF;
    if(ext4_fseek(&fd->f,fd->off,whence) == EOK)
        return offset;
    else{return -1;}
}


uint64_t sys_open(const char *path, int flags, unsigned int mode) {
    proc_t *proc = Schedule::this_proc();

    fd_t *fd = (fd_t*)kmalloc(sizeof(fd));
    fd->flags = flags;
    fd->off = 0;
    fd->path = path;
    ext4_fopen(&fd->f,path,flags);

    if (!fd) return (uint64_t)((int64_t)-1);
    proc->fd_table[proc->fd_count++] = fd;
    return proc->fd_count - 1;
}
