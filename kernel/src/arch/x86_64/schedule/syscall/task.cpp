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
uint64_t sys_pmmap(
    uint64_t pid, uint64_t mode,
    uint64_t proc_addr, uint64_t tproc_addr, uint64_t flag, uint64_t length
) {
    (void)proc_addr; // We Just Not Need this yet

    if (length == 0) return -EINVAL;
    if (!Schedule::this_proc()->IsTrusted) return -EPERM;

    // 1. 查找目标进程
    spinlock_lock(&PID2PROC_TREE_LOCK);
    proc_t *proc = (proc_t*)art_search(pid2proc_tree, (const uint8_t*)&pid, 8);
    spinlock_unlock(&PID2PROC_TREE_LOCK);
    if (!proc) return -ESRCH;

    // 2. 计算需要的页数
    uint64_t pages = (mode == 0) ? DIV_ROUND_UP(length, PAGE_SIZE) : length;
    if (pages == 0) return -EINVAL;

    // 确保分配给用户态的内存带有用户态标志
    uint64_t flags = flag | MM_USER;

    // 3. 如果不指定地址，让 VMM 自动寻找空洞并映射
    if (tproc_addr == 0) {
        void *ret = VMM::EAlloc(proc->pagemap, pages, flags);
        if (!ret) return -ENOMEM;
        return (uint64_t)ret;
    }

    // 4. 如果指定了地址 (tproc_addr != 0)，需要在特定地址映射
    if (!is_user_address(tproc_addr) || (tproc_addr & (PAGE_SIZE - 1))) {
        return -EFAULT; // 必须页对齐且在用户态空间
    }

    // 锁定目标进程的 VMA 树和页表，防止并发冲突
    spinlock_lock(&proc->pagemap->vma_lock);
    spinlock_lock(&proc->pagemap->pt_lock);

    // 修复：使用 IsRangeFree 检查整个请求区间是否存在任何重叠
    if (!VMM::VMA::IsRangeFree(proc->pagemap, tproc_addr, pages)) {
        spinlock_unlock(&proc->pagemap->pt_lock);
        spinlock_unlock(&proc->pagemap->vma_lock);
        return -EEXIST;
    }

    // 预留虚拟地址空间
    VMM::VMA::AddRegion(proc->pagemap, tproc_addr, pages, flags);

    uint64_t mapped = 0;
    uint64_t cur_v = tproc_addr;

    // 分配物理内存并建立页表映射
    while (mapped < pages) {
        void *phys = PMM::Request();
        if (!phys) {
            // OOM 发生，必须回滚：解除已映射的部分并释放物理内存
            uint64_t rollback_v = tproc_addr;
            while (rollback_v < cur_v) {
                uint64_t p = VMM::GetPhysics(proc->pagemap, rollback_v);
                if (p) {
                    PMM::Free((void*)p);
                    VMM::UnmapNoFlush(proc->pagemap, rollback_v);
                }
                rollback_v += PAGE_SIZE;
            }
            VMM::LazyTLB::ShootdownFull(proc->pagemap);
            
            // 移除刚才预留的 VMA 区域
            vma_region_t *r = VMM::VMA::FindRegion(proc->pagemap, tproc_addr);
            if (r) VMM::VMA::RemoveRegion(proc->pagemap,r);

            spinlock_unlock(&proc->pagemap->pt_lock);
            spinlock_unlock(&proc->pagemap->vma_lock);
            return -ENOMEM;
        }

        VMM::Map4K(proc->pagemap, cur_v, (uint64_t)phys, flags);
        cur_v += PAGE_SIZE;
        mapped++;
    }

    // 将这次映射记录到 vm_mappings 链表，方便未来 Free
    VMM::NewMapping(proc->pagemap, tproc_addr, pages, flags);

    spinlock_unlock(&proc->pagemap->pt_lock);
    spinlock_unlock(&proc->pagemap->vma_lock);

    return tproc_addr;
}