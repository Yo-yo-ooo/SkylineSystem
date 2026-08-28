// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/art.h>
#include <atomic/atomic.h>
#include <arch/x86_64/lapic/lapic.h>

extern art_tree *pid2proc_tree;

uint64_t sys_getpid(GENERATE_IGN6()) {
    IGNV_6();
    return Schedule::this_proc()->id;
}

uint64_t sys_gettid(GENERATE_IGN6()) {
    IGNV_6();
    return Schedule::this_thread()->id;
}

uint64_t sys_exit(uint64_t code,GENERATE_IGN5()) {
    IGNV_5();
    Schedule::Exit((int32_t)code);
    return 0;
}

uint64_t sched_yield(GENERATE_IGN6()){
    IGNV_6();

    Schedule::Yield();
    return 0;
}

extern spinlock_t PID2PROC_TREE_LOCK;
uint64_t sys_kill(uint64_t pid,uint64_t sig, GENERATE_IGN4()) {
    IGNV_4();

    spinlock_lock(&PID2PROC_TREE_LOCK);
    proc_t *proc = (proc_t*)art_search(pid2proc_tree,(const uint8_t*)&pid,8);
    spinlock_unlock(&PID2PROC_TREE_LOCK);
    if (!proc) return -ESRCH;

    asm volatile("cli");    // 必须关中断，保证切换过程绝对原子
    LAPIC::StopTimer();
    Schedule::PROC_KILL(proc);

    return 0;
}

uint64_t sys_fork(syscall_frame_t *frame){
    Schedule::PAUSE();
    proc_t* proc = Schedule::ForkProcess();
    if (!proc) { Schedule::Resume(); return (uint64_t)-1; }
    thread_t *thread = Schedule::ForkThread(proc, Schedule::this_thread(), frame);
    if (!thread) { Schedule::Resume(); return (uint64_t)-1; }
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

/* ============================================================
 * sys_pmmapSHARE — 返回值与寄存器通道契约 (正式 ABI):
 *
 *   rax (return value):
 *     >= 0  成功 (历史语义, 不要依赖具体值)
 *     <  0  -errno 失败
 *
 *   rdi (sideband, 仅成功时有效): resolved_src — src 进程侧 VA
 *   rsi (sideband, 仅成功时有效): resolved_dst — dst 进程侧 VA
 *
 *    失败时 rdi/rsi 保证为 0 (内核在所有出口清零)。
 *   调用方判 rax<0 必须丢弃 rdi/rsi; 即使忘了判,
 *   0 地址也会让误用立刻 #PF 在明处, 而非静默用旧值。
 *
 *   ⚠ 使用约束: syscall 硬件只保证 rax/rcx/r11;
 *   rdi/rsi 依赖本内核 syscall_entry stub 从栈帧恢复全部 GPR。
 *   调用方必须在 syscall 返回后、任何其他函数调用之前锁存。
 * ============================================================ */
uint64_t sys_pmmapSHARE(
    uint64_t dst_pid, uint64_t dst_addr, uint64_t length,
    uint64_t flags,   uint64_t src_pid,  uint64_t src_addr,syscall_frame_t* frame
) {
    /* ---------- 0. 契约初始化: 任何出口前先清零侧带通道 ----------
     * 此后所有 return 路径(成功/失败)语义一致:
     * rdi/rsi 非零 ⇔ 本次调用成功且为本次的双侧地址 */
    volatile syscall_frame_t *vframe = frame;
    vframe->rdi = 0;
    vframe->rsi = 0;

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

    /* flags 归一化: 三套体系 (PROT_/VMM_FLAG_/MM_) 的 bit0/bit1 恰好同值。
       调用方至少要传 bit0 (读/P位) — 只传 bit1 (写) 会得到 P=0 的幽灵 PTE */
    uint64_t map_flags = flags | MM_USER;
    if (!(map_flags & MM_READ)) map_flags |= MM_READ;

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
     *  不能用 Alloc/EAlloc —— 它们会分配新物理页,
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
            VMM::Internal::PageInfo si =
                VMM::Internal::GetPageInfo(src_pm, resolved_src + off);
            if (si.size == 0 || (si.flags & VMM_COW_BIT)) { ok = false; goto unlock; }
        }
    }

    /* ---------- 4d. 核心: 穿透映射 ----------
     * 遍历 src 区间, 取每页的物理帧, 用相同粒度(1G/2M/4K)填进 dst 的
     * 页表 + RefSharedPhys 计数。dst 的 PTE 与 src 的 PTE 指向同一物理帧:
     *   A 写 → 落到物理帧; B 读 → 从同一物理帧取 (缓存一致性保证互通) */
    for (uint64_t off = 0; off < size; ) {
        VMM::Internal::PageInfo si =
            VMM::Internal::GetPageInfo(src_pm, resolved_src + off);
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

    /* ---------- 成功: 填充侧带通道 (rdi=src VA, rsi=dst VA) ---------- */
    vframe->rdi = resolved_src;
    vframe->rsi = resolved_dst;

unlock:
    if (pm_b != pm_a) {
        spinlock_unlock(&pm_b->pt_lock);
        spinlock_unlock(&pm_b->vma_lock);
    }
    spinlock_unlock(&pm_a->pt_lock);
    spinlock_unlock(&pm_a->vma_lock);

    /* ---------- 5. Phase 3: 失败回滚 (VMM::Free 自取锁 → 必须在双锁外) ----------
     * rdi/rsi 保持入口清零状态 — 失败侧带必为 0 (契约保证) */
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

    /* rax: 成功 (>=0)。双侧地址经 rdi/rsi 侧带通道返回 */
    if (me == DestProc) return resolved_dst;
    return resolved_src;
}

extern uint64_t sched_tid;
extern uint32_t sched_prio_to_weight[16];

uint64_t sys_thread_launch(uint64_t entry, uint64_t hint, GENERATE_IGN4()){

    IGNV_4();
    proc_t *me = Schedule::this_proc();
    thread_t *curr = Schedule::this_thread();
    if (!me || !curr) return -EPERM;
    if (!is_user_address(entry)) return -EINVAL;    /* 入口必须是用户地址 */

    /* hint: UINT64_MAX 内核选核, 否则指定核 */
    cpu_t *cpu;
    if (hint != UINT64_MAX && hint < MAX_CPU && smp_cpu_list[hint])
        cpu = smp_cpu_list[hint];
    else
        cpu = get_lw_cpu();
    if (!cpu) return -EFAULT;

    thread_t *t = (thread_t*)kmalloc(sizeof(thread_t));
    if (!t) return -ENOMEM;
    _memset(t, 0, sizeof(thread_t));
    t->state = THREAD_RUNNING;              

    t->id = atomic_add_fetch_8(&sched_tid, 1, ATOMIC_RELAXED);
    t->timer_cpu = cpu->id;
    t->cpu_num = cpu->id;
    t->parent = me;
    t->pagemap = me->pagemap;                /* 共享地址空间 = 线程的本质 */
    t->priority = curr->priority;
    t->weight = sched_prio_to_weight[t->priority];
    uint64_t base_vruntime = cpu->avg_vruntime;
    uint64_t half_slice = cpu->base_quantum / 2;
    t->vruntime = base_vruntime > half_slice ? base_vruntime - half_slice : 0;


    t->fx_area = (char*)VMM::Alloc(kernel_pagemap,
                            DIV_ROUND_UP(cpu->XsaveSize, PAGE_SIZE), false);
    if (!t->fx_area) { kfree(t); return -ENOMEM; }
    _memset(t->fx_area, 0, cpu->XsaveSize);
    cpu->OverLoadableFuncs.StoreSIMDState(t->fx_area,
                                          cpu->XsaveMaskLo, cpu->XsaveMaskHi);

    /* 内核栈 */
    uint64_t kstack = (uint64_t)VMM::Alloc(kernel_pagemap, 4, false);
    if (!kstack) { VMM::Free(kernel_pagemap, t->fx_area); kfree(t); return -ENOMEM; }
    _memset((void*)kstack, 0, 4 * PAGE_SIZE);
    t->kernel_stack = kstack;
    t->kernel_rsp = kstack + PAGE_SIZE * 4;

    /*  用户栈 — 内核分配 (无 stack 参数的代价, 内核必须管):
       在调用者的 pagemap 里分配, 8 页 (NewThread 同款) */
    uint64_t ustack = (uint64_t)VMM::Alloc(me->pagemap, 8, true);
    if (!ustack) {
        VMM::Free(kernel_pagemap, (void*)kstack);
        VMM::Free(kernel_pagemap, t->fx_area);
        kfree(t);
        return -ENOMEM;
    }
    t->stack = ustack;

    /* 用户上下文 — 无 arg, rsp 指栈顶对齐 */
    t->ctx.rip = entry;
    t->ctx.rsp = (ustack + 8 * PAGE_SIZE) & ~0xFULL;
    t->ctx.rdi = 0;                          /* 定义死: arg == 0 */
    t->ctx.cs = 0x23; t->ctx.ss = 0x1b; t->ctx.rflags = 0x202;
    t->thread_stack = t->ctx.rsp;

    /* ---- 一切就绪, 最后一刻发射 (此后 t 不可再碰) ---- */
    Schedule::Internal::ProcessAddThread(me, t);

    uint64_t rflags = spin_lock_irqsave(&cpu->sched_lock);
    cpu->has_runnable_thread = true;
    Schedule::Internal::InsertToQueue(cpu, t);
    spin_unlock_irqrestore(&cpu->sched_lock, rflags);

    /* 唤醒目标核 (launch 语义: 注册即启动, 核可能睡着不知道队列有货) */
    cpu_t *self = this_cpu();
    if (cpu != self) LAPIC::IPI(cpu->lapic_id, SCHED_VEC + 1);

    return t->id;
}