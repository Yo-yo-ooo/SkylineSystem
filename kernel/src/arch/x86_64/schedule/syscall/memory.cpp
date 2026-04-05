/*
* SPDX-License-Identifier: GPL-2.0-only
* File: memory.cpp
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
#include <klib/algorithm/queue.h>


uint64_t sys_mmap_(void *addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd,uint64_t offset) {
    
    proc_t *process = Schedule::this_proc();
    pagemap_t *pagemap = process->pagemap;
    
    if(process->fd_table[fd]->path == NULL){
        return -EINVAL;
    }
    size_t pages = DIV_ROUND_UP(length, PAGE_SIZE);
    
    uint64_t flags_vm = 0;
    if(prot & PROT_READ) flags_vm |= MM_READ;
    if(prot & PROT_WRITE) flags_vm |= MM_WRITE;
    if(prot & PROT_EXEC) flags_vm |= MM_NX;
    flags_vm |= MM_USER;
    spinlock_lock(&pagemap->vma_lock);
    uint64_t Address = 0;
    if(!addr){
        Address = VMM::Useless::InternalAlloc(pagemap, pages, flags_vm);
    }else{Address = (uint64_t)addr;}
    for (int32_t i = 0; i < pages; i++)
        VMM::Map(pagemap, Address + (i * PAGE_SIZE), (uint64_t)PMM::Request(), flags_vm);
    VMM::NewMapping(pagemap, Address, pages, flags_vm);
    spinlock_unlock(&pagemap->vma_lock);
    if(flags & MAP_SHARED){
        ReadFile(process->fd_table[fd],Address,length);
    }
    return Address;
}

uint64_t sys_mmap(uint64_t addr_,uint64_t length, uint64_t prot, \
    uint64_t flags, uint64_t fd,uint64_t offset){
    return sys_mmap_((void*)addr_,length,prot,flags,fd,offset);
}

uint64_t sys_munmap(uint64_t addr, uint64_t length, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2,uint64_t ign_3){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);
    
    size_t pages = DIV_ROUND_UP(length, PAGE_SIZE);
    for (size_t i = 0; i < pages; i++) {
        VMM::Unmap(Schedule::this_thread()->pagemap, (uint64_t)addr - i * PAGE_SIZE);
    }
    return 0;
}

uint64_t sys_brk(uint64_t addr, GENERATE_IGN5()) {
    IGNV_5();
    thread_t *t = Schedule::this_thread();
    pagemap_t *pagemap = t->pagemap;

    // 1. 基础检查与 brk(0) 处理
    uint64_t current_brk = (uint64_t)t->heap + t->heap_size;
    if (addr == 0) return current_brk;
    if (addr < (uint64_t)t->heap) return current_brk;

    uint64_t old_page_count = DIV_ROUND_UP(t->heap_size, PAGE_SIZE);
    uint64_t new_size = addr - (uint64_t)t->heap;
    uint64_t new_page_count = DIV_ROUND_UP(new_size, PAGE_SIZE);

    if (new_page_count > old_page_count) {
        // 2. 扩容逻辑
        for (uint64_t i = old_page_count; i < new_page_count; i++) {
            uint64_t vaddr = (uint64_t)t->heap + (i * PAGE_SIZE);
            uint64_t paddr = (uint64_t)PMM::Request(); 
            if (!paddr) return current_brk;

            // 映射并清理内存 
            VMM::Map(pagemap, vaddr, paddr, MM_USER | MM_WRITE);
            _memset(HIGHER_HALF((void*)paddr), 0, PAGE_SIZE);
        }
    } 
    else if (new_page_count < old_page_count) {
        // 3. 缩容逻辑：绕过失效的 VMM::Free，直接操作单页
        for (uint64_t i = new_page_count; i < old_page_count; i++) {
            uint64_t vaddr = (uint64_t)t->heap + (i * PAGE_SIZE);
            uint64_t paddr = VMM::GetPhysics(pagemap, vaddr);
            
            if (paddr) {
                VMM::Unmap(pagemap, vaddr);
                PMM::Free((void*)paddr); // 还给物理内存管理器
                mmu_invlpg(vaddr);
            }
        }
    }

    // 4. 同步 VMA 链表状态
    // 否则 Fork 或退出时会因为 page_count 不匹配导致解映射错误
    if (t->heap_vma) {
        t->heap_vma->page_count = new_page_count;
    }

    t->heap_size = new_size;
    return (uint64_t)t->heap + t->heap_size; // 返回新的断点
}

uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);

    thread_t * tthread = Schedule::this_thread();
    size_t pages = DIV_ROUND_UP(len, PAGE_SIZE);
    uint64_t vm_flags = 0;
    if (prot & 2) 
        vm_flags |= MM_READ;
    if (prot & 4) 
        vm_flags |= MM_WRITE;
    if (!(prot & 1)) 
        vm_flags |= MM_NX;

    spinlock_lock(&tthread->pagemap->vma_lock);
    VMM::MapRange(tthread->pagemap, addr, 0, vm_flags, pages);
    spinlock_unlock(&tthread->pagemap->vma_lock);

    return 0;
}