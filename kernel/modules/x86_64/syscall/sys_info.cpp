//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* sys_info.cpp —— sys_sysinfo (slot 16) 实现，迁入 kernel.syscall.info.ko。
 * 信息页本体仍由 Tier0 sysinfo.cpp 持有（init/idle_refresh 被内核调用），
 * 本 .ko 只通过 sysinfo_kernel_* 访问器共享页指针/页数/就绪状态。 */
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/schedule/sysinfo.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/pit/pit.h>
#include <mem/pmm.h>
#include <klib/errno.h>
#include <klib/klib.h>
#include <klib/kio.h>

extern bool sysinfo_kernel_ready(void);
extern SysInfo *sysinfo_kernel_page(void);
extern uint32_t sysinfo_kernel_npages(void);

uint64_t sys_sysinfo(uint64_t arg, GENERATE_IGN5()){
    IGNV_5();

    proc_t *me = Schedule::this_proc();
    if (!me) return (uint64_t)(-EPERM);
    pagemap_t *pm = (pagemap_t*)me->pagemap;
    if (!pm) return (uint64_t)(-EFAULT);

    if (!sysinfo_kernel_ready())
        return (uint64_t)(-ENODEV);
    SysInfo *kinfo = sysinfo_kernel_page();
    uint32_t kinfo_npages = sysinfo_kernel_npages();

    /* ==================== 释放路径 (arg > 0) ==================== */
    if (arg != 0) {
        uint64_t va = arg;
        if (va & 0xFFF) return (uint64_t)(-EINVAL);

        bool have = false;
        spinlock_lock(&pm->vma_lock);
        vma_region_t *r = VMM::VMA::FindRegion(pm, va);
        if (r && r->start == va && r->page_count == kinfo_npages)
            have = true;
        spinlock_unlock(&pm->vma_lock);
        if (!have)
            return (uint64_t)(-EINVAL);

        uint64_t kphys0 = VMM::GetPhysics((pagemap_t*)kernel_pagemap,
                                           (uint64_t)kinfo);
        uint64_t uphys0 = VMM::GetPhysics(pm, va);
        if (!kphys0 || uphys0 != kphys0)
            return (uint64_t)(-EINVAL);

        spinlock_lock(&pm->pt_lock);
        for (uint32_t i = 0; i < kinfo_npages; i++)
            VMM::UnmapNoFlush(pm, va + (uint64_t)i * 4096);
        spinlock_unlock(&pm->pt_lock);
        VMM::LazyTLB::ShootdownFull(pm);

        spinlock_lock(&pm->vma_lock);
        r = VMM::VMA::FindRegion(pm, va);
        if (r && r->start == va && r->page_count == kinfo_npages)
            VMM::VMA::RemoveRegion(pm, r);
        spinlock_unlock(&pm->vma_lock);
        kinfoln("OK! REMOVE SYSINFO MAPPING!");

        return 0;
    }

    /* ==================== 映射路径 (arg == 0) ==================== */
    __atomic_store_n(&kinfo->mem_free, PMM::FreePages() << 12, __ATOMIC_RELAXED);
    __atomic_store_n(&kinfo->uptime_ms, PIT::TimeSinceBootMS(), __ATOMIC_RELAXED);

    uint64_t va = 0;
    {
        spinlock_lock(&pm->vma_lock);
        va = VMM::VMA::InternalAlloc(pm, kinfo_npages,
                                     MM_READ | MM_USER, 0);
        if (!va) {
            spinlock_unlock(&pm->vma_lock);
            return (uint64_t)(-ENOMEM);
        }
        if (!VMM::VMA::FindRegion(pm, va))
            VMM::VMA::AddRegion(pm, va, kinfo_npages,
                                MM_READ | MM_USER);
        spinlock_unlock(&pm->vma_lock);
    }

    spinlock_lock(&pm->pt_lock);
    for (uint32_t i = 0; i < kinfo_npages; i++) {
        uint64_t kv = (uint64_t)kinfo + (uint64_t)i * 4096;
        uint64_t phys = VMM::GetPhysics((pagemap_t*)kernel_pagemap, kv);
        if (unlikely(!phys)) {
            spinlock_unlock(&pm->pt_lock);
            return (uint64_t)(-ENODEV);
        }
        VMM::Map4K(pm, va + (uint64_t)i * 4096, phys,
                   MM_READ | MM_USER);
    }
    spinlock_unlock(&pm->pt_lock);

    return va;
}
