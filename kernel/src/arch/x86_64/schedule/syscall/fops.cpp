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

extern volatile bool IsPM5LVL;
uint64_t sys_fread(uint64_t fd_idx, uint64_t buf, uint64_t count, \
uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan, fd_idx);
    if(!FD) { return -EBADF; }
    
    if (count == 0) return 0;
    
    uint64_t user_space_end = IsPM5LVL ? USER_SPACE_END_5LVL : USER_SPACE_END_4LVL;
    if (buf >= user_space_end) return -EFAULT;
    if (count > user_space_end - buf) return -EFAULT;

    void* kbuf = kmalloc(count);
    size_t total_read = 0;
    //kinfoln("READING %u BYTES",count);
    FD->FSOPS->read(FD->filedesc,kbuf,count,&total_read);
    VMM::UserAccess::CopyToUser(proc->pagemap,buf,kbuf,count);

    //kinfoln("READED %u bytes successfully!", total_read);
    return (int64_t)total_read;
}

uint64_t sys_fsize(uint64_t fd_idx,GENERATE_IGN5()){
    IGNV_5();
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd_idx);
    if(!FD){return -EBADF;}
    return FD->FSOPS->fsize(FD->filedesc);
}

uint64_t sys_fwrite(uint64_t fd_idx, uint64_t buf, uint64_t count, \
uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd_idx);
    if(!FD){return -EBADF;}
    // 安全校验：如果 count 为 0，直接返回 0
    if (count == 0) return 0;
    
    // 安全校验：检查用户指针是否越界进入内核区域
    if (!is_user_buffer_valid(buf, count)) {
        return -EFAULT; // Bad address
    }

    size_t wcnt = 0;
    int32_t status = FD->FSOPS->write(FD->filedesc, (void*)buf, count, &wcnt);
    
    if (status != 0) return (int64_t)status; // 返回负数错误码
    return (int64_t)wcnt;

    //return FD->FSOPS->write(FD->filedesc,buf,count);
}

uint64_t sys_flseek(uint64_t fd_idx, uint64_t offset, uint64_t whence, \
uint64_t ign_0,uint64_t ign_1,uint64_t ign_2){
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd_idx);
    if(!FD){return -EBADF;}
    
    // 直接将偏移指令透传给内核
    // 增加 whence 的有效性检查
    if (whence > 2) {return -EINVAL; }
    return FD->FSOPS->lseek(FD->filedesc,offset,whence);
}

// 优化后的路径长度检测：利用 64 位并行扫描
static inline bool is_path_too_long(const char* kpath) {
    const uint64_t* p = (const uint64_t*)kpath;
    const uint64_t* end = (const uint64_t*)(kpath + PATH_MAX);

    // 每次处理 8 个字节
    while (p < end) {
        uint64_t v = *p;
        
        // 核心技巧：SWAR (SIMD Within A Register)
        // 检测 64 位字中是否有任意一个字节为 0
        // (v - 0x0101010101010101) & ~v & 0x8080808080808080
        if (((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL)) {
            return false; // 找到了 '\0'，路径合法
        }
        p++;
    }
    return true; // 没找到 '\0'，路径超长
}


// 辅助函数：安全地从用户态拷贝字符串 (遇到 \0 停止)，返回拷贝的字节数，失败返回负数
static int64_t strncpy_from_user(char* dst, const char* src, size_t max_len, pagemap_t* pagemap) {
    for (size_t i = 0; i < max_len; i++) {
        if (!VMM::UserAccess::CopyFromUser(pagemap, dst + i, (void*)(src + i), 1)) {
            return -EFAULT; // 越界
        }
        if (dst[i] == '\0') {
            return i; // 成功找到结尾
        }
    }
    return -ENAMETOOLONG; // 超过 max_len 仍未找到 \0
}
extern void P(ext4_file *f);
uint64_t sys_fopen(uint64_t path, uint64_t flags, GENERATE_IGN4()) {
    proc_t *proc = Schedule::this_proc();

    if (!is_user_address(path)) { 
        return -EFAULT; 
    }

    char *kpath = (char *)kmalloc(PATH_MAX);
    if (!kpath) { 
        return -ENOMEM; 
    }


    // [修复 1] 替换强制 PATH_MAX 拷贝为安全字符串拷贝
    int64_t path_len = strncpy_from_user(kpath, (const char*)path, PATH_MAX, proc->pagemap);
    if (path_len < 0) {
        kinfoln("sys_fopen free kpath(pathlen < 0)");
        kfree(kpath);
        return path_len; // 可能是 -EFAULT 或 -ENAMETOOLONG
    }
    //kinfoln("KAPTH:%s",kpath);
    __hmap_s_mp *MP = GetMount(kpath);
    if (!MP) {
        kinfoln("sys_fopen free kpath(!MP)");
        kfree(kpath);
        return -ENOENT; 
    }

    fd_t *fd_struct;
    int32_t fd_idx = fd_alloc(proc->FDMan, &fd_struct);
    if (fd_idx < 0) {
        kfree(kpath);
        return -EMFILE; 
    }

    fd_struct->FSOPS = MP->FSOPS;
    fd_struct->MP = MP;
    fd_struct->filedesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
    _memset(fd_struct->filedesc,0,MP->FSOPS->SIZEOF_FILE_DESC);
    
    int32_t err = MP->FSOPS->open(fd_struct->filedesc, kpath, flags);

    //ext4_file *f = (ext4_file*)fd_struct->filedesc;
    //P(f);
    

    //kinfoln("%d",MP->FSOPS->SIZEOF_FILE_DESC);

    kfree(kpath); 

    if (err < 0) {
        kfree(fd_struct->filedesc);
        fd_free(proc->FDMan, fd_idx);
        return err; 
    }

    return (uint64_t)fd_idx;
}

uint64_t sys_fclose(uint64_t fd,GENERATE_IGN5()){
    IGNV_5();
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd);
    if(!FD){return -EBADF;}
    int32_t res = FD->FSOPS->close(FD->filedesc);
    fd_free(proc->FDMan,fd);
    return res;
}

uint64_t sys_mkdir(uint64_t path,uint64_t mode,GENERATE_IGN4()){
    IGNV_4();
    
}

