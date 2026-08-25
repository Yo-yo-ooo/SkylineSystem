//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/art.h>
#include <arch/x86_64/lapic/lapic.h>

extern art_tree *pid2proc_tree;

uint64_t sys_getpid(uint64_t ign_0, uint64_t ign_1, uint64_t ign_2, \
    uint64_t ign_3,uint64_t ign_4,uint64_t ign_5) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(ign_5);

    return Schedule::this_proc()->id;
}

uint64_t sys_gettid(uint64_t ign_0, uint64_t ign_1, uint64_t ign_2, \
    uint64_t ign_3,uint64_t ign_4,uint64_t ign_5) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(ign_5);

    return Schedule::this_thread()->id;
}



uint64_t sys_exit(uint64_t code,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);

    Schedule::Exit((int32_t)code);

    return 0;
}

uint64_t sched_yield(uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3,uint64_t ign_4,uint64_t ign_5){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);IGNORE_VALUE(ign_4);IGNORE_VALUE(ign_5);

    Schedule::Yield();
    return 0;
}


extern spinlock_t PID2PROC_TREE_LOCK;
uint64_t sys_kill(uint64_t pid,uint64_t sig, uint64_t ign_0, \
    uint64_t ign_1,uint64_t ign_2,uint64_t ign_3) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);

    asm volatile("cli");    // 必须关中断，保证切换过程绝对原子
    LAPIC::StopTimer();
    proc_t *proc = (proc_t*)art_search(pid2proc_tree,(const uint8_t*)&pid,8);
    Schedule::PROC_KILL(proc);

    return 0;
}

uint64_t sys_fork(syscall_frame_t *frame){
    Schedule::PAUSE();
    proc_t* proc = Schedule::ForkProcess();
    thread_t *thread = Schedule::ForkThread(proc, Schedule::this_thread(), frame);
    Schedule::Resume();
    return proc->id;
}

// Map Memory For Process(Use Target Process PageMap)
#ifndef USER_ADDR_LIMIT
#define USER_ADDR_LIMIT 0x0000800000000000ULL
#endif
#ifndef PMM_2M_PAGES
#define PMM_2M_PAGES 512
#endif

uint64_t sys_pmmapSHARE(
    uint64_t dst_pid, uint64_t dst_addr, uint64_t length,
    uint64_t flags,   uint64_t src_pid,  uint64_t src_addr
) {
    /* ---------- 1. 校验 ---------- */
    if (length == 0) return -EINVAL;
    proc_t *me = Schedule::this_proc();
    if (!me || !me->IsTrusted) return -EPERM;

    if ((src_addr != 0 && (src_addr & (PAGE_SIZE - 1))) ||
        (dst_addr != 0 && (dst_addr & (PAGE_SIZE - 1))) ||
        (length & (PAGE_SIZE - 1)))
        return -EINVAL;

    uint64_t pages = length / PAGE_SIZE;
    uint64_t size  = pages * PAGE_SIZE;
    if (pages == 0) return -EINVAL;

    if ((dst_addr != 0 && (dst_addr >= USER_ADDR_LIMIT || dst_addr + size < dst_addr)) ||
        (src_addr != 0 && (src_addr >= USER_ADDR_LIMIT || src_addr + size < src_addr)))
        return -EINVAL;

    uint64_t map_flags = flags | MM_USER;

    /* ---------- 2. 解析进程 ---------- */
    spinlock_lock(&PID2PROC_TREE_LOCK);
    proc_t *SrcProc  = (proc_t*)art_search(pid2proc_tree, (const uint8_t*)&src_pid, 8);
    proc_t *DestProc = (proc_t*)art_search(pid2proc_tree, (const uint8_t*)&dst_pid, 8);
    spinlock_unlock(&PID2PROC_TREE_LOCK);
    if (!SrcProc || !DestProc) return -ESRCH;
    if (SrcProc->exiting || DestProc->exiting) return -ESRCH;

    pagemap_t *src_pm = SrcProc->pagemap;
    pagemap_t *dst_pm = DestProc->pagemap;
    if (!src_pm || !dst_pm) return -EINVAL;
    if (src_pm == (pagemap_t*)kernel_pagemap ||
        dst_pm == (pagemap_t*)kernel_pagemap) return -EPERM;   /* kmalloc 锁序 */

    const bool src_new = (src_addr == 0);
    const bool dst_new = (dst_addr == 0);
    uint64_t resolved_src = src_addr;
    uint64_t resolved_dst = dst_addr;

    /* ---------- 3. Phase 1: src 侧 (EAlloc 自取锁 → 必须在双锁前) ----------
     * src_addr==0: EAlloc 分 VA + 物理页 + 登记 VMA, 一步到位。
     * 之后这段是"src 侧的实体", dst 侧仅做穿透映射。 */
    if (src_new) {
        void *p = VMM::EAlloc(src_pm, pages, map_flags | VMM_SHARED_BIT);
        if (!p) return -ENOMEM;
        resolved_src = (uint64_t)p;
    }

    /* ---------- 4. Phase 2: 双 pagemap 加锁 ---------- */
    pagemap_t *pm_a = (src_pm < dst_pm) ? src_pm : dst_pm;
    pagemap_t *pm_b = (src_pm < dst_pm) ? dst_pm : src_pm;
    spinlock_lock(&pm_a->vma_lock);
    spinlock_lock(&pm_a->pt_lock);
    if (pm_b != pm_a) {
        spinlock_lock(&pm_b->vma_lock);
        spinlock_lock(&pm_b->pt_lock);
    }

    bool ok = true;

    /* 4a. src 侧: 登记 + 标记共享 */
    if (src_new) {
        /* 防御性: EAlloc/InternalAlloc 若未登记区域则补上 */
        if (!VMM::VMA::FindRegion(src_pm, resolved_src))
            VMM::VMA::AddRegion(src_pm, resolved_src, pages,
                                map_flags | VMM_SHARED_BIT);
    } else {
        /* src 指定区间: 标记所有相交 VMA 区域 + 要求完全覆盖
         * (未登记区域的页退出时无人释放 → 共享 = 永久泄漏) */
        uint64_t covered = 0;
        if (src_pm->vma_head) {
            vma_region_t *vr = src_pm->vma_head;
            do {
                uint64_t vs = vr->start;
                uint64_t ve = vs + (uint64_t)vr->page_count * PAGE_SIZE;
                uint64_t lo = (vs > resolved_src) ? vs : resolved_src;
                uint64_t hi = (ve < resolved_src + size) ? ve : resolved_src + size;
                if (lo < hi) { vr->flags |= VMM_SHARED_BIT; covered += hi - lo; }
                vr = vr->next;
            } while (vr != src_pm->vma_head);
        }
        if (covered != size) { ok = false; goto unlock; }
    }

    /* 4b. dst 侧: 区间就位 (仅 VA, 物理页来自 src)
     * ★ 不能用 Alloc/EAlloc —— 它们会分配新物理页,
     *   随后被穿透映射覆盖 → 物理页孤儿泄漏 */
    if (dst_new) {
        resolved_dst = VMM::VMA::InternalAlloc(dst_pm, pages,
                                               map_flags | VMM_SHARED_BIT, 0);
        if (!resolved_dst) { ok = false; goto unlock; }
        if (!VMM::VMA::FindRegion(dst_pm, resolved_dst))
            VMM::VMA::AddRegion(dst_pm, resolved_dst, pages,
                                map_flags | VMM_SHARED_BIT);
    } else {
        /* 调用方指定 dst 区间: 无 VMA 重叠 + PTE 完全未映射 */
        if (dst_pm->vma_head) {
            vma_region_t *vr = dst_pm->vma_head;
            do {
                uint64_t vs = vr->start;
                uint64_t ve = vs + (uint64_t)vr->page_count * PAGE_SIZE;
                if (resolved_dst < ve && vs < resolved_dst + size) {
                    ok = false; goto unlock;
                }
                vr = vr->next;
            } while (vr != dst_pm->vma_head);
        }
        for (uint64_t off = 0; off < size; off += PAGE_SIZE)
            if (VMM::GetPhysics(dst_pm, resolved_dst + off) != 0) {
                ok = false; goto unlock;
            }
        VMM::VMA::AddRegion(dst_pm, resolved_dst, pages,
                            map_flags | VMM_SHARED_BIT);
    }

    /* 4c. src 校验: 完全映射 + 拒绝 CoW (只穿透现成的、无主的页) */
    if (!src_new) {
        for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
            VMM::Useless::PageInfo si =
                VMM::Useless::GetPageInfo(src_pm, resolved_src + off);
            if (si.size == 0 || (si.flags & VMM_COW_BIT)) { ok = false; goto unlock; }
        }
    }

    /* ---------- 4d. 核心: 穿透映射 ----------
     * 遍历 src 区间, 取每页的物理帧, 用相同粒度(1G/2M/4K)填进 dst 的
     * 页表 + RefSharedPhys 计数。dst 的 PTE 与 src 的 PTE 指向同一物理帧:
     *   A 写 → 落到物理帧; B 读 → 从同一物理帧取 (缓存一致性保证互通) */
    for (uint64_t off = 0; off < size; ) {
        VMM::Useless::PageInfo si =
            VMM::Useless::GetPageInfo(src_pm, resolved_src + off);
        uint64_t sv = resolved_src + off;
        uint64_t dv = resolved_dst  + off;
        uint64_t phys, chunk;

        if (si.size == PAGE_1GB &&
            (sv & (PAGE_1GB - 1)) == 0 && (dv & (PAGE_1GB - 1)) == 0 &&
            size - off >= PAGE_1GB) {
            chunk = PAGE_1GB; phys = si.phys;
            VMM::Map1G(dst_pm, dv, phys, map_flags);
            for (uint64_t k = 0; k < PAGE_1GB / PAGE_SIZE; k++)
                RefSharedPhys(phys + k * PAGE_SIZE);
        } else if (si.size >= PAGE_2MB &&
                   (sv & (PAGE_2MB - 1)) == 0 && (dv & (PAGE_2MB - 1)) == 0 &&
                   size - off >= PAGE_2MB) {
            chunk = PAGE_2MB; phys = si.phys + (sv & (si.size - 1));
            VMM::Map2M(dst_pm, dv, phys, map_flags);
            for (uint64_t k = 0; k < PMM_2M_PAGES; k++)
                RefSharedPhys(phys + k * PAGE_SIZE);
        } else {
            chunk = PAGE_SIZE; phys = si.phys + (sv & (si.size - 1));
            VMM::Map4K(dst_pm, dv, phys, map_flags);
            RefSharedPhys(phys);
        }
        off += chunk;
    }

    /* 4e. dst 侧 vm_mapping 登记 (CleanPM 需要走 FreeSharedRegion 递减引用) */
    VMM::NewMapping(dst_pm, resolved_dst, pages, map_flags | VMM_SHARED_BIT);

unlock:
    if (pm_b != pm_a) {
        spinlock_unlock(&pm_b->pt_lock);
        spinlock_unlock(&pm_b->vma_lock);
    }
    spinlock_unlock(&pm_a->pt_lock);
    spinlock_unlock(&pm_a->vma_lock);

    /* ---------- 5. Phase 3: 失败回滚 (VMM::Free 自取锁 → 必须在双锁外) ---------- */
    if (!ok) {
        if (src_new) {
            spinlock_lock(&src_pm->vma_lock);
            vma_region_t *sr = VMM::VMA::FindRegion(src_pm, resolved_src);
            if (sr) sr->flags &= ~VMM_SHARED_BIT;
            spinlock_unlock(&src_pm->vma_lock);
            VMM::Free(src_pm, (void*)resolved_src);
        }
        if (dst_new && resolved_dst != 0)
            VMM::Free(dst_pm, (void*)resolved_dst);
        return -EINVAL;
    }

    /* 返回: 双方各自的 VA 不同很正常 —— 共享的是物理帧, 不是虚拟地址。
     * 调用方 == dst 返回 dst 侧 VA, 否则 src 侧 VA。
     * 对方侧 VA 由调用方自行经 IPC 传递 (一进程一返回值的天然限制)。 */
    if (me == DestProc) return resolved_dst;
    return resolved_src;
}