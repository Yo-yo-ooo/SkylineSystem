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

    /* 修复: mode==2 时 page_count 直接等于用户传入的 length, 无任何上限,
       可请求天文数字页抽干 PMM。统一钳制 1GB(2^18 页)。 */
    if (unlikely(page_count > (1u << 18))) {
        return SYS_MAP_FAILED;
    }

    uint64_t ret = (uint64_t)VMM::Alloc(pagemap, page_count, true);
    if (ret == (uint64_t)SYS_MAP_FAILED || ret == 0) {
        return SYS_MAP_FAILED;
    }

    if (!VMM::VMA::FindRegion(pagemap, ret)) {
        VMM::VMA::AddRegion(pagemap, ret, page_count, MM_READ | MM_WRITE | MM_USER);
    }

    { proc_t* mp = Schedule::this_proc();
      uint64_t pid = mp ? mp->id : 63;
      static uint32_t s_mmseq = 0;
      if (s_mmseq < 300)
          kprintf("MM seq=%lu pid=%lu pc=%lu\n",
                  (unsigned long)s_mmseq, (unsigned long)pid,
                  (unsigned long)page_count);
      s_mmseq++;
      PMM::dbg_mcharge(pid, (int64_t)page_count); }
    return ret;
}

uint64_t sys_mmap(uint64_t addr_,uint64_t length, uint64_t mode, \
    uint64_t flags,uint64_t offset,uint64_t ign_0,syscall_frame_t *nullframe){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(nullframe);
    return sys_mmap_((void*)addr_,length,mode,flags,offset);
}

uint64_t sys_munmap(uint64_t addr, uint64_t length,
    GENERATE_IGN4())
{
    IGNV_4();
    proc_t *me = Schedule::this_proc();
    pagemap_t *pm = me ? me->pagemap : (pagemap_t*)kernel_pagemap;
    if(!pm) return -EFAULT;

    /* 修复: 原实现忽略 length 释放整个 VMA 区域 —— 用户态分配器按页
       局部归还时, 会把 MergeRegion 合并进来的活跃邻居一并释放,
       导致仍在使用的物理页被回收(二次分配/数据破坏)。
       改为按 length 拆分区域, 只释放 [addr, addr+len) 部分。 */
    uint64_t pages = length / PAGE_SIZE;
    if (length % PAGE_SIZE) pages++;
    if (pages == 0) return 0;
    if (addr % PAGE_SIZE) return -EINVAL;
    uint64_t end = addr + pages * PAGE_SIZE;
    if (end < addr) return -EINVAL; /* 溢出 */

    spinlock_lock(&pm->vma_lock);
    vma_region_t *r = VMM::VMA::FindRegion(pm, addr);
    if (!r || addr < r->start) {
        spinlock_unlock(&pm->vma_lock);
        return -EINVAL;
    }
    uint64_t region_end = r->start + r->page_count * PAGE_SIZE;
    if (end < region_end) {
        /* 尾部保留: 拆分后 VMM::Free 只释放头部区域 */
        VMM::VMA::SplitRegion(pm, r, end);
    }
    spinlock_unlock(&pm->vma_lock);

    VMM::Free(pm, (void*)addr);
    { proc_t* mp = Schedule::this_proc();
      PMM::dbg_mcharge(mp ? mp->id : 63, -(int64_t)pages); }
    return 0;
}

uint64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot, \
    GENERATE_IGN3()) {
    IGNV_3();
    (void)addr; (void)len; (void)prot;
    /* 修复: 原实现 (1) PROT 位映射颠倒 (2) MapRange(addr, 0) 会把物理 0
       映射进用户空间 (3) 从未注册。正确实现需处理巨页拆分, 在此之前
       一律返回 -ENOSYS, 消除误用风险(待豆包实现并注册)。 */
    return -ENOSYS;
}