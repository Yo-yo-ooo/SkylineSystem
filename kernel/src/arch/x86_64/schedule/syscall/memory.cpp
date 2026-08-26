//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>
#include <arch/x86_64/vmm/vmm.h>

#define SYS_MAP_FAILED ((uint64_t)-1ULL)

extern spinlock_t pmm_lock;
extern "C" void mmu_invlpg(uint64_t vaddr);

uint64_t sys_mmap_(void *addr, uint64_t length, uint64_t mode, uint64_t flags, uint64_t offset) {
    if (__builtin_expect(length == 0, 0)) {
        return SYS_MAP_FAILED;
    }

    thread_t *current_thread = Schedule::this_thread();
    if (__builtin_expect(!current_thread || !current_thread->pagemap, 0)) {
        return SYS_MAP_FAILED;
    }

    pagemap_t *pagemap = current_thread->pagemap;
    uint64_t page_count = 0;

    if (mode == 2) {
        page_count = length;
    } else if (mode == 3) {
        page_count = DIV_ROUND_UP(length, PAGE_SIZE);
    } else {
        return SYS_MAP_FAILED;
    }

    uint64_t ret = VMM::Alloc(pagemap, page_count, true);
    if (ret == (uint64_t)SYS_MAP_FAILED || ret == 0) {
        return SYS_MAP_FAILED;
    }

    if (!VMM::VMA::FindRegion(pagemap, ret)) {
        VMM::VMA::AddRegion(pagemap, ret, page_count, MM_READ | MM_WRITE | MM_USER);
    }

    return ret;
}

uint64_t sys_mmap(uint64_t addr_,uint64_t length, uint64_t mode, \
    uint64_t flags,uint64_t offset){
    return sys_mmap_((void*)addr_,length,mode,flags,offset);
}

uint64_t sys_munmap(uint64_t addr, uint64_t length,
    uint64_t ign_0, uint64_t ign_1, uint64_t ign_2, uint64_t ign_3)
{
    IGNORE_VALUE(ign_0);
    IGNORE_VALUE(ign_1);
    IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);

    proc_t *me = Schedule::this_proc();
    pagemap_t *pm = me ? me->pagemap : (pagemap_t*)kernel_pagemap;
    if(!pm) return -EFAULT;
    VMM::Free(pm, (void*)addr);

    return 0;
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