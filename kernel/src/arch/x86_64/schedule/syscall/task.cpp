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
    uint64_t flags,   uint64_t src_pid, uint64_t src_addr
) {
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

    spinlock_lock(&PID2PROC_TREE_LOCK);
    proc_t *SrcProc  = (proc_t*)art_search(pid2proc_tree, (const uint8_t*)&src_pid, 8);
    proc_t *DestProc = (proc_t*)art_search(pid2proc_tree, (const uint8_t*)&dst_pid, 8);
    spinlock_unlock(&PID2PROC_TREE_LOCK);
    if (!SrcProc || !DestProc) return -ESRCH;
    if (SrcProc->exiting || DestProc->exiting) return -ESRCH;

    pagemap_t *src_pm = SrcProc->pagemap;
    pagemap_t *dst_pm = DestProc->pagemap;
    if (!src_pm || !dst_pm) return -EINVAL;

    /* 双 pagemap 加锁: 指针序防 ABBA; 同一 pagemap 只锁一次 */
    pagemap_t *pm_a = (src_pm < dst_pm) ? src_pm : dst_pm;
    pagemap_t *pm_b = (src_pm < dst_pm) ? dst_pm : src_pm;
    spinlock_lock(&pm_a->vma_lock);
    spinlock_lock(&pm_a->pt_lock);
    if (pm_b != pm_a) {
        spinlock_lock(&pm_b->vma_lock);
        spinlock_lock(&pm_b->pt_lock);
    }

    bool ok = true;
    uint64_t resolved_src = src_addr;
    uint64_t resolved_dst = dst_addr;
    uint64_t mapped = 0;

    /* ---- src_addr==0: 在 src 用户半区分配新段 + 新物理页 ---- */
    if (src_addr == 0) {
        resolved_src = VMM::VMA::InternalAlloc(src_pm, pages, map_flags, 0);
        if (!resolved_src) { ok = false; goto done; }

        uint64_t v = resolved_src, remain = size;
        while (remain > 0) {
            if (remain >= 524288 * PAGE_SIZE && (v & (PAGE_2GB - 1)) == 0) {
                void *p = PMM::Request2GB();
                if (p) {
                    uint64_t ph = (uint64_t)p;
                    VMM::Map1G(src_pm, v, ph, map_flags);
                    VMM::Map1G(src_pm, v + PAGE_1GB, ph + PAGE_1GB, map_flags);
                    remain -= PAGE_2GB; v += PAGE_2GB; continue;
                }
            }
            if (remain >= 512 * PAGE_SIZE && (v & (PAGE_2MB - 1)) == 0) {
                void *p = PMM::Request2MB();
                if (p) {
                    VMM::Map2M(src_pm, v, (uint64_t)p, map_flags);
                    remain -= PAGE_2MB; v += PAGE_2MB; continue;
                }
            }
            void *p = PMM::Request();
            if (!p) { ok = false; break; }
            VMM::Map4K(src_pm, v, (uint64_t)p, map_flags);
            remain -= PAGE_SIZE; v += PAGE_SIZE;
        }
        if (!ok) {
            VMM::Free(src_pm, (void*)resolved_src);
            goto done;
        }
        VMM::VMA::AddRegion(src_pm, resolved_src, pages, map_flags | VMM_SHARED_BIT);
        VMM::NewMapping(src_pm, resolved_src, pages, map_flags | VMM_SHARED_BIT);
    } else {
        /* 标记 src 的既有区域为共享 (CleanPM 走 FreeSharedRegion 路径) */
        vma_region_t *sr = VMM::VMA::FindRegion(src_pm, resolved_src);
        if (!sr) { ok = false; goto done; }
        sr->flags |= VMM_SHARED_BIT;
        vm_mapping_t *sm = src_pm->vm_mappings;
        if (sm) {
            vm_mapping_t *start_m = sm;
            do {
                if (sm->start == resolved_src) { sm->flags |= VMM_SHARED_BIT; break; }
                sm = sm->next;
            } while (sm != start_m);
        }
    }

    /* ---- dst_addr==0: 在 dst 分配虚拟段 ---- */
    if (dst_addr == 0) {
        resolved_dst = VMM::VMA::InternalAlloc(dst_pm, pages, map_flags, 0);
        if (!resolved_dst) {
            if (src_addr == 0) VMM::Free(src_pm, (void*)resolved_src);
            ok = false; goto done;
        }
    } else {
        /* 校验 dst 区间完全未映射 (避免覆盖既有映射导致物理页泄漏) */
        for (uint64_t off = 0; off < size; off += PAGE_SIZE)
            if (VMM::GetPhysics(dst_pm, resolved_dst + off) != 0) {
                if (src_addr == 0) VMM::Free(src_pm, (void*)resolved_src);
                ok = false; goto done;
            }
    }

    /* ---- 校验 src 已映射 + 非 CoW (拒绝 CoW 页, 避免写共享污染 fork 父子) ---- */
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        VMM::Useless::PageInfo si = VMM::Useless::GetPageInfo(src_pm, resolved_src + off);
        if (si.size == 0 || (si.flags & VMM_COW_BIT)) {
            if (src_addr == 0) VMM::Free(src_pm, (void*)resolved_src);
            ok = false; goto done;
        }
    }

    /* ---- 映射 src 的物理页到 dst + 引用计数++ ---- */
    
    for (uint64_t off = 0; off < size; ) {
        VMM::Useless::PageInfo si = VMM::Useless::GetPageInfo(src_pm, resolved_src + off);
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
        off += chunk; mapped += chunk;
    }

    /* 登记 dst VMA (VMM_SHARED_BIT → CleanPM 走 FreeSharedRegion 逐 4K 递减) */
    VMM::VMA::AddRegion(dst_pm, resolved_dst, pages, map_flags | VMM_SHARED_BIT);
    VMM::NewMapping(dst_pm, resolved_dst, pages, map_flags | VMM_SHARED_BIT);

done:
    if (pm_b != pm_a) {
        spinlock_unlock(&pm_b->pt_lock);
        spinlock_unlock(&pm_b->vma_lock);
    }
    spinlock_unlock(&pm_a->pt_lock);
    spinlock_unlock(&pm_a->vma_lock);
    return ok ? (src_addr == 0 ? resolved_src : resolved_dst) : (uint64_t)(-EINVAL);
}