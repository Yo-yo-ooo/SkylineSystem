/*
* SPDX-License-Identifier: GPL-2.0-only
* File: fops.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
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
    FileSystemOps[fd->FsType].read(fd,(void*)buf,count);
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
    FileSystemOps[fd->FsType].write(fd,(void*)buf,count);
    fd->off += count;
    return count;
}

int64_t sys_lseek(uint64_t fd_idx, uint64_t offset, uint64_t whence, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2){
    fd_t *fd = Schedule::this_proc()->fd_table[fd_idx];
    if (!fd)
        return -EBADF;
    if(FileSystemOps[fd->FsType].lseek(fd,offset,whence) == EOK)
        return offset;
    else{return -1;}
}


uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode, \
    GENERATE_IGN3()) {
    IGNV_3();
    proc_t *proc = Schedule::this_proc();

    fd_t *fd = (fd_t*)kmalloc(sizeof(fd));
    fd->flags = (int32_t)flags;
    fd->off = 0;
    fd->path = (char*)path;

    
    /* ext4_fopen(&fd->f,path,flags); */
    FileSystemOps[FSType::FS_EXT4].open(fd,(const char*)path,flags);
    fd->FsType = FSType::FS_EXT4;
    fd->Type = 0; //Generic File
return_:
    if (!fd) return (uint64_t)((int64_t)-1);
    proc->fd_table[proc->fd_count++] = fd;
    return proc->fd_count - 1;
}

uint64_t sys_close(uint64_t fd,GENERATE_IGN5()){
    IGNV_5();
    proc_t *proc = Schedule::this_proc();
    if(fd >= (uint64_t)proc->fd_count)
        return (uint64_t)((int64_t)-1);
    else{
        if(proc->fd_table[fd]->Type == 0){ //Generic File
            FileSystemOps[proc->fd_table[fd]->FsType].close(proc->fd_table[fd]);
        }
    }
    proc->fd_table[fd] = nullptr;
    return 0;
}

uint64_t sys_mkdir(uint64_t path,uint64_t mode,GENERATE_IGN4()){
    IGNV_4();
    if(ext4_dir_mk((const char*)path) != EOK)
        return (uint64_t)((int64_t)-1);
    if(mode != NULL)
        if(ext4_mode_set(path,(uint32_t)mode) != EOK)
        return (uint64_t)((int64_t)-1);
    return 0;
}

