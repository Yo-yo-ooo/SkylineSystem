//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// sysinfo.cpp — 实时共享只读信息页 + 映射 syscall + idle 刷新
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/schedule/sysinfo.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/vmm/vmm.h>
#include <mem/pmm.h>
#include <klib/algorithm/art.h>
#include <klib/errno.h>

extern art_tree *pid2proc_tree;
extern uint64_t sched_tid;

/* started_count = 成功上线的 AP 数 (BSP 不经过 smp_cpu_init,
     smp_init 的等待条件 started_count < cpu_count - 1 印证),
     真实核数 = started_count + 1 (补 BSP) */
extern volatile uint64_t started_count;

/* LAPIC→逻辑核号表 — SIMD 掩码枚举与调度器同源 */
extern uint8_t apic_id_to_logical[256];

static_assert(MAX_CPU <= SYSINFO_MAX_CPUS,
              "kernel MAX_CPU exceeds SysInfo page cap");

/* ==================== 页本体 ====================
    VMM::EAlloc 直接分配 — VA/物理页/VMA 一次到位:
     - 内核侧: kinfo 指针直接读写 (EAlloc 的返回 VA)
     - 用户侧: GetPhysics 反查帧, 逐页只读穿透
   没有帧表, 没有 staging, 没有手动 PMM::Request */
static SysInfo *kinfo    = nullptr;   /* EAlloc 返回的内核 VA */
static uint32_t kinfo_npages = 0;     /* 页数 (= kinfo->occupy_pages) */
static bool     kinfo_ready = false;

/* ==================== CPUID 辅助 ==================== */
static inline void sysinfo_cpuid(uint32_t leaf,
                                 uint32_t *a, uint32_t *b,
                                 uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(0));
}

static void sysinfo_fill_brand(char out[48]) {
    uint32_t regs[12];
    uint32_t a, b, c, d;

    sysinfo_cpuid(0x80000000, &a, &b, &c, &d);
    if (a < 0x80000004) {
        const char fallback[] = "unknown";
        _memset(out, 0, 48);
        __memcpy(out, fallback, sizeof(fallback));
        return;
    }
    for (int i = 0; i < 3; i++) {
        sysinfo_cpuid(0x80000002 + (uint32_t)i,
                      &regs[i*4], &regs[i*4+1],
                      &regs[i*4+2], &regs[i*4+3]);
    }
    __memcpy(out, regs, 48);
    out[47] = 0;
}

/* ============================================================
 * sys_sysinfo_init — SMP 就绪后、首个用户进程前调用一次
 *
 *   调用点: x86_64_init 中 smp_init() 返回之后
 *   (所有 AP 的 simd_cpu_init 已完成 — smp_init 内部等待
 *    started_count == cpu_count-1, 掩码全部就绪)
 * ============================================================ */
void sys_sysinfo_idle_refresh(void);
void sys_sysinfo_init(void) {
    /* ---- CPU 静态信息 ---- */
    /*  ncpus = started_count + 1:
         started_count 只数 AP (BSP 不进 smp_cpu_init,
         等待条件 < cpu_count-1 为证), +1 补 BSP */
    uint32_t ncpus = (uint32_t)started_count + 1;
    if (ncpus > SYSINFO_MAX_CPUS) ncpus = SYSINFO_MAX_CPUS;
    if (ncpus == 0) ncpus = 1;            /* 防御: 永不该到 0 */

    uint32_t npages = (uint32_t)SYSINFO_PAGES(ncpus);
    if (npages == 0) npages = 1;

    /* ----  VMM 一把分配 ---- */
    kinfo = (SysInfo*)VMM::EAlloc((pagemap_t*)kernel_pagemap,
                                  npages, MM_READ | MM_WRITE);
    if (!kinfo) {
        kinfo_ready = false;
        return;                  /* 分配失败: 优雅降级, syscall 报 -ENODEV */
    }
    kinfo_npages = npages;

    /* EAlloc 物理页来自 PMM, 不保证清零 — 防御性清零
       (sys_mmap 泄漏案的同源教训) */
    _memset(kinfo, 0, SYSINFO_SIZE(ncpus));

    /* ---- 身份 + 映射元信息 ---- */
    kinfo->occupy_pages  = npages;              /*  用户第一个读的字段 */
    kinfo->magic         = SYSINFO_MAGIC;
    kinfo->abi_version   = SYSINFO_ABI_VER;
    kinfo->kernel_version= 0;

    /* ---- CPU ---- */
    kinfo->ncpus = ncpus;

    uint32_t a, b, c, d;
    sysinfo_cpuid(1, &a, &b, &c, &d);
    kinfo->cpu_features_edx = d;
    kinfo->cpu_features_ecx = c;
    kinfo->cpu_family = (((a >> 20) & 0xFF) << 20) |
                        (((a >> 8)  & 0xF)  << 16) |
                        (((a >> 16) & 0xF)  << 12) |
                        (((a >> 4)  & 0xF)  << 4)  |
                         ((a)       & 0xF);
    sysinfo_fill_brand(kinfo->cpu_brand);

    /* ---- 内存总量 ---- */
    kinfo->mem_total = PMM::pmm_bitmap_pages << 12;

    /* ----  SIMD 掩码: apic_id_to_logical 枚举 — 与调度器同源 ----
       LAPIC ID 0..255 逐个查表: 有效逻辑号且 cpu 存活 → 掩码入槽。
       表本身就是 LAPIC→logical 的权威映射, 逻辑号即下标。
       logical >= ncpus 的越界防御吸收"逻辑号不连续"的未来变化 */
    for (uint32_t lapic = 0; lapic < 256; lapic++) {
        uint32_t logical = (uint32_t)apic_id_to_logical[lapic];
        if (logical == 0xFF) continue;
        if (logical >= MAX_CPU) continue;
        if (logical >= ncpus) continue;         /* 越界防御 */

        cpu_t *cpu = smp_cpu_list[logical];
        if (cpu)
            kinfo->simd_mask[logical] = cpu->simd_mask;
    }

    kinfo_ready = true;

    /* ---- 首次动态刷新 ---- */
    sys_sysinfo_idle_refresh();
}

/* ============================================================
 * sys_sysinfo_idle_refresh — idle 线程周期调用 (动态字段)
 * 挂点: sched_idle 循环, 每个 hlt 唤醒周期一次
 * ============================================================ */
void sys_sysinfo_idle_refresh(void) {
    if (!kinfo_ready) return;

    __atomic_store_n(&kinfo->mem_free,
                     PMM::FreePages() << 12, __ATOMIC_RELAXED);
    __atomic_store_n(&kinfo->mem_used,
                     kinfo->mem_total - kinfo->mem_free,
                     __ATOMIC_RELAXED);

    __atomic_store_n(&kinfo->uptime_ms,
                     PIT::TimeSinceBootMS(), __ATOMIC_RELAXED);

    __atomic_store_n(&kinfo->nprocs,
                     (uint64_t)pid2proc_tree->size, __ATOMIC_RELAXED);
    __atomic_store_n(&kinfo->nthreads_approx,
                     sched_tid, __ATOMIC_RELAXED);

    uint64_t total_cs = 0;
    uint32_t n = kinfo->ncpus;
    for (uint32_t i = 0; i < n; i++) {
        cpu_t *cpu = smp_cpu_list[i];
        if (cpu) total_cs += cpu->sched_stats.context_switches;
    }
    __atomic_store_n(&kinfo->ctx_switches, total_cs, __ATOMIC_RELAXED);
}

/* ============================================================
 * sys_sysinfo — 零参数, 返回用户侧连续只读映射起始 VA
 *
 *   用户:  SysInfo *si = (SysInfo*)sys_sysinfo();
 *          之后直接读 — 无 syscall 无拷贝无锁
 *   只读由 PTE 保证 (无 MM_WRITE), 用户写入即 #PF
 * ============================================================ */
uint64_t sys_sysinfo(GENERATE_IGN6())
{
    IGNV_6();

    proc_t *me = Schedule::this_proc();
    if (!me) return (uint64_t)(-EPERM);

    if (!kinfo_ready || !kinfo)
        return (uint64_t)(-ENODEV);

    pagemap_t *pm = (pagemap_t*)me->pagemap;
    if (!pm) return (uint64_t)(-EFAULT);

    /* 用户侧挑连续 npages 的 VA 槽 */
    uint64_t va = VMM::VMA::InternalAlloc(pm, kinfo_npages,
                                           MM_READ | MM_USER, 0);
    if (!va) return (uint64_t)(-ENOMEM);

    /*  逐页只读穿透 — 物理帧经 GetPhysics 从 VMM 反查 */
    for (uint32_t i = 0; i < kinfo_npages; i++) {
        uint64_t kv = (uint64_t)kinfo + (uint64_t)i * 4096;
        uint64_t phys = VMM::GetPhysics((pagemap_t*)kernel_pagemap, kv);
        if (unlikely(!phys)) {
            /* 理论不可达 (EAlloc 已映射) — 防御中止 */
            return (uint64_t)(-ENODEV);
        }
        VMM::Map4K(pm, va + (uint64_t)i * 4096, phys,
                   MM_READ | MM_USER);
    }

    /* VMA 登记 — sys_mmap 的教训: 不登记则 Free/CleanPM 语义不完整 */
    if (!VMM::VMA::FindRegion(pm, va))
        VMM::VMA::AddRegion(pm, va, kinfo_npages,
                            MM_READ | MM_USER);

    return va;
}