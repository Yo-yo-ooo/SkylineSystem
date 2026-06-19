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



uint64_t sys_fread(uint64_t fd_idx, uint64_t buf, uint64_t count, \
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
    return FD->FSOPS->read(FD->filedesc,buf,count);
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
    return FD->FSOPS->write(FD->filedesc,buf,count);
}

uint64_t sys_flseek(uint64_t fd_idx, uint64_t offset, uint64_t whence, \
uint64_t ign_0,uint64_t ign_1,uint64_t ign_2){
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd_idx);
    if(!FD){return -EBADF;}
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


uint64_t sys_fopen(uint64_t path, uint64_t flags, uint64_t mode, GENERATE_IGN3()) {
    proc_t *proc = Schedule::this_proc();

    // 基础地址合法性校验
    if (!is_user_address(path)) { 
        return -EFAULT; 
    }

    // 动态分配内核缓冲区，防止内核栈溢出
    char *kpath = (char *)kmalloc(PATH_MAX);
    if (!kpath) { 
        return -ENOMEM; // 内存不足
    }

    // 安全地将路径从用户态拷贝到内核态 (最多拷 PATH_MAX 字节)
    if (!VMM::UserAccess::CopyFromUser(proc->pagemap, kpath, (void*)path, PATH_MAX)) {
        kfree(kpath);
        return -EFAULT; // 用户态地址存在未映射或越界
    }
    
    if (is_path_too_long(kpath)) {
        kfree(kpath);
        return -ENAMETOOLONG; // 抛出错误：File name too long
    }

    //  挂载点路由查找 (此时使用安全的内核 kpath)
    __hmap_s_mp *MP = GetMount(kpath);
    if (!MP) {
        kfree(kpath);
        return -ENOENT; // 找不到挂载点或文件
    }

    // ==========================================
    // 以下是完成 fopen 的标准文件描述符分配逻辑
    // ==========================================

    // 在当前进程的 FD 管理器中分配一个空闲槽位
    fd_t *fd_struct;
    int32_t fd_idx = fd_alloc(proc->FDMan, &fd_struct);
    if (fd_idx < 0) {
        kfree(kpath);
        return -EMFILE; // 错误：Too many open files (进程 FD 已用尽)
    }

    // 初始化 FD 结构体，并调用底层文件系统真实的 open 函数
    fd_struct->FSOPS = MP->FSOPS;
    fd_struct->MP = MP;
    
    // 假设底层 open 函数会将具体的文件控制块指针存入 fd_struct->filedesc
    int32_t err = MP->FSOPS->open(fd_struct->filedesc, kpath, flags);

    // 路径解析完毕，可以释放内核态字符串内存了
    kfree(kpath); 

    // 如果底层文件系统打开失败，必须回滚/回收刚分配的 FD
    if (err < 0) {
        fd_free(proc->FDMan, fd_idx);
        return err; // 返回底层文件系统抛出的具体错误码
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

