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

/* vmm.h 需可见:
 *   void  RefSharedPhys(uint64_t phys);
 *   void *EAlloc(pagemap_t *pm, uint64_t page_count, uint64_t flags);
 *   以及 InternalAlloc / AddRegion / NewMapping / GetPageInfo / Map1G/2M/4K / Free */

uint64_t sys_pmmapSHARE(
    uint64_t dst_pid, uint64_t dst_addr, uint64_t length,
    uint64_t flags,   uint64_t src_pid,  uint64_t src_addr
) {
    /* ========== 1. 参数校验 ========== */
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

    uint64_t map_flags = flags | MM_USER;   // 调用方需自备 MM_READ/MM_WRITE

    /* ========== 2. 解析进程 ========== */
    spinlock_lock(&PID2PROC_TREE_LOCK);
    proc_t *SrcProc  = (proc_t*)art_search(pid2proc_tree, (const uint8_t*)&src_pid, 8);
    proc_t *DestProc = (proc_t*)art_search(pid2proc_tree, (const uint8_t*)&dst_pid, 8);
    spinlock_unlock(&PID2PROC_TREE_LOCK);
    if (!SrcProc || !DestProc) return -ESRCH;
    if (SrcProc->exiting || DestProc->exiting) return -ESRCH;

    pagemap_t *src_pm = SrcProc->pagemap;
    pagemap_t *dst_pm = DestProc->pagemap;
    if (!src_pm || !dst_pm) return -EINVAL;

    const bool src_new = (src_addr == 0);
    const bool dst_new = (dst_addr == 0);
    uint64_t resolved_src = src_addr;
    uint64_t resolved_dst = dst_addr;

    /* ========== 3. Phase 1: src_addr==0 → VMM::EAlloc ==========
     * 一站式: VA 区间 + 物理页(大页优先) + VMA 登记 + vm_mapping 登记,
     * 取代原手写的 InternalAlloc + Request2GB/2MB/4K + Map 循环。
     * flags 带 VMM_SHARED_BIT → 区域登记为共享, src 退出时 CleanPM
     * 走 FreeSharedRegion (逐 4K 引用递减) 而非 FreeOwnedRegion。
     * (bit56 落在 PTE_KEEP 内, 进 PTE 无害, 同 COW_BIT 的用法)
     *
     * ★ EAlloc 内部自取 vma_lock/pt_lock —— 必须在双锁阶段之前调用,
     *   否则在自己已持有的锁上自旋死锁 */
    if (src_new) {
        void *p = VMM::EAlloc(src_pm, pages, map_flags | VMM_SHARED_BIT);
        if (!p) return -ENOMEM;
        resolved_src = (uint64_t)p;
    }

    /* ========== 4. Phase 2: 双 pagemap 加锁 ========== */
    pagemap_t *pm_a = (src_pm < dst_pm) ? src_pm : dst_pm;
    pagemap_t *pm_b = (src_pm < dst_pm) ? dst_pm : src_pm;
    spinlock_lock(&pm_a->vma_lock);
    spinlock_lock(&pm_a->pt_lock);
    if (pm_b != pm_a) {
        spinlock_lock(&pm_b->vma_lock);
        spinlock_lock(&pm_b->pt_lock);
    }

    bool ok = true;

    /* ---- 4a. src 既有区间: 标记所有相交的 VMA 区域 ----
     * (区间可能跨多个区域, 逐个标记; 同时校验区间被 VMA 完全覆盖 ——
     *  未登记区域的页在进程退出时本就无人释放, 共享它会造成永久泄漏) */
    if (!src_new) {
        uint64_t covered = 0;
        if (src_pm->vma_head) {
            vma_region_t *vr = src_pm->vma_head;
            do {
                uint64_t vs = vr->start;
                uint64_t ve = vs + (uint64_t)vr->page_count * PAGE_SIZE;
                uint64_t is = (vs > resolved_src) ? vs : resolved_src;
                uint64_t ie = (ve < resolved_src + size) ? ve : resolved_src + size;
                if (is < ie) {
                    vr->flags |= VMM_SHARED_BIT;
                    covered += ie - is;
                }
                vr = vr->next;
            } while (vr != src_pm->vma_head);
        }
        if (covered != size) { ok = false; goto unlock; }
        /* 注: vm_mapping->flags 无任何读取方, 不再维护 */
    }

    /* ---- 4b. dst 区间就位 ---- */
    if (dst_new) {
        /* dst 只需要 VA 区间 —— 物理页来自 src!
         * 不能用 VMM::Alloc/EAlloc: 它们会分配全新物理页,
         * 随后被 src 的物理页覆盖 → 凭空泄漏 N 个页。
         * InternalAlloc 不自取锁 (vma_lock 由调用方持有,
         * VMM::Alloc 内部正是如此使用), 双锁下调用安全。 */
        resolved_dst = VMM::VMA::InternalAlloc(dst_pm, pages,
                                               map_flags | VMM_SHARED_BIT, 0);
        if (!resolved_dst) { ok = false; goto unlock; }
        /* ★ 不再 AddRegion —— InternalAlloc 已登记区域。
         *   旧版此处二次登记会打坏 VMA 结构, 本版修复 */
    } else {
        /* 调用方指定区间: 不得与已登记 VMA 区域重叠 (重叠 = VMA 结构损坏) */
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
        /* 且必须完全未映射 (覆盖既有 PTE → 物理页泄漏) */
        for (uint64_t off = 0; off < size; off += PAGE_SIZE)
            if (VMM::GetPhysics(dst_pm, resolved_dst + off) != 0) {
                ok = false; goto unlock;
            }
    }

    kinfoln("HIT!");

    /* ---- 4c. 校验 src 已映射 + 非 CoW ----
     * (src_new 路径 EAlloc 刚建好映射且无 CoW, 天然满足, 跳过整趟遍历) */
    if (!src_new) {
        for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
            VMM::Useless::PageInfo si =
                VMM::Useless::GetPageInfo(src_pm, resolved_src + off);
            if (si.size == 0 || (si.flags & VMM_COW_BIT)) {   // 拒绝 CoW 页
                ok = false; goto unlock;
            }
        }
    }

    /* ---- 4d. 穿透映射 src 物理页 → dst + 引用计数 ---- */
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

    /* ---- 4e. dst 侧登记 ---- */
    if (!dst_new)
        VMM::VMA::AddRegion(dst_pm, resolved_dst, pages, map_flags | VMM_SHARED_BIT);
    VMM::NewMapping(dst_pm, resolved_dst, pages, map_flags | VMM_SHARED_BIT);
    /* dst_new: 区域已由 InternalAlloc 登记, 只补 vm_mapping 即可 */

unlock:
    if (pm_b != pm_a) {
        spinlock_unlock(&pm_b->pt_lock);
        spinlock_unlock(&pm_b->vma_lock);
    }
    spinlock_unlock(&pm_a->pt_lock);
    spinlock_unlock(&pm_a->vma_lock);

    /* ========== 5. Phase 3: 失败回滚 (双锁之外 —— VMM::Free 自取锁) ========== */
    if (!ok) {
        if (src_new) {
            /* 本调用创建的区域, 且所有失败点都位于 RefSharedPhys 之前
             * (零引用建立) → 摘 SHARED 标记走 FreeOwnedRegion 大页快路径 */
            spinlock_lock(&src_pm->vma_lock);
            vma_region_t *sr = VMM::VMA::FindRegion(src_pm, resolved_src);
            if (sr) sr->flags &= ~VMM_SHARED_BIT;
            spinlock_unlock(&src_pm->vma_lock);
            VMM::Free(src_pm, (void*)resolved_src);
        }
        /* !src_new: 保留已打的 SHARED 标记 —— RefDecPhys 对未入树的页
         * 返回 true (正常释放), 功能正确, 只是该区域将来退出时大页
         * 退化为逐 4K 释放。反过来清除才有风险: 无法区分这标记是本次
         * 打的还是先前共享调用打的, 误清会让先前的共享方 UAF */
        if (dst_new && resolved_dst != 0) {
            VMM::Free(dst_pm, (void*)resolved_dst);   /* 空区域, 仅摘登记 */
        }
        return -EINVAL;
    }

    /* 返回约定: src_addr==0 返回 src 侧新分配地址, 否则返回 dst 地址 */
    return src_new ? resolved_src : resolved_dst;
}