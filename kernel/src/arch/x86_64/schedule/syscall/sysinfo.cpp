//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/pit/pit.h>
#include <mem/pmm.h>
#include <klib/algorithm/art.h>
#include <klib/errno.h>
#include <arch/x86_64/schedule/sysinfo.h>


/* ---- CPUID 辅助 ---- */
static inline void sysinfo_cpuid(uint32_t leaf, uint32_t sub,
                                 uint32_t *a, uint32_t *b,
                                 uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid"
                      : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(sub));
}

/* 品牌串: 0x80000002..04 三次调用, 48 字节 */
static void sysinfo_fill_brand(char out[48]) {
    uint32_t regs[12];
    uint32_t a, b, c, d;

    sysinfo_cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000004) {                       /* 不支持品牌串 */
        const char fallback[] = "unknown";
        for (int i = 0; i < 48; i++) out[i] = 0;
        __memcpy(out, fallback, sizeof(fallback));
        return;
    }
    for (int i = 0; i < 3; i++) {
        sysinfo_cpuid(0x80000002 + i, 0,
                      &regs[i*4+0], &regs[i*4+1],
                      &regs[i*4+2], &regs[i*4+3]);
    }
    __memcpy(out, regs, 48);
    out[47] = 0;                                 /* 强制零结尾 */
}

extern art_tree *pid2proc_tree;
extern uint64_t sched_tid;
uint64_t sys_sysinfo(uint64_t buf, uint64_t buflen, GENERATE_IGN4()){
    IGNV_4();

    proc_t *me = Schedule::this_proc();
    if (!me) return -EPERM;

    if (!is_user_address(buf)) return -EFAULT;
    if (!is_user_buffer_valid(buf, buflen)) return -EFAULT;
    if (buflen < sizeof(SysInfo)) return -EINVAL;

    SysInfo *si = (SysInfo*)buf;
    _memset(si, 0, sizeof(SysInfo));             /* 预留区/unfilled 恒 0 */

    /* ---- 身份 ---- */
    si->magic         = SYSINFO_MAGIC;
    si->abi_version   = SYSINFO_ABI_VER;
    si->kernel_version= 0;

    /* ---- CPU ---- */
    si->ncpus = (uint32_t)(smp_last_cpu + 1);

    uint32_t a, b, c, d;
    sysinfo_cpuid(1, 0, &a, &b, &c, &d);
    si->cpu_features_edx = d;
    si->cpu_features_ecx = c;
    si->cpu_family = (((a >> 20) & 0xFF) << 20) |   /* ext family */
                     (((a >> 8)  & 0xF)  << 16) |    /* family */
                     (((a >> 16) & 0xF)  << 12) |    /* ext model */
                     (((a >> 4)  & 0xF)  << 4)  |    /* model */
                      ((a)       & 0xF);             /* stepping */

    sysinfo_fill_brand(si->cpu_brand);

    /* ---- 每核 SIMD 域 ---- */
    uint32_t n = si->ncpus;
    if (n > MAX_CPU) n = MAX_CPU;
    for (uint32_t i = 0; i < n; i++) {
        cpu_t *cpu = smp_cpu_list[i];
        si->simd_mask[i] = cpu ? cpu->simd_mask : 0;
    }

    /* ---- 内存 — PMM O(1) 计数器接入 ---- */
    si->mem_total = PMM::pmm_bitmap_pages << 12;
    si->mem_free  = PMM::FreePages() << 12;
    si->mem_used  = si->mem_total - si->mem_free;

    /* ---- 运行时 ---- */
    si->uptime_ms = PIT::TimeSinceBootMS();

    si->nprocs = (uint64_t)pid2proc_tree->size;      /* ART 树节点数 ≈ 存活进程 */
    
    si->nthreads_approx = sched_tid;                 /* 近似: 含已退出 — 注记在头文件 */

    uint64_t total_cs = 0;
    for (uint32_t i = 0; i < n; i++) {
        cpu_t *cpu = smp_cpu_list[i];
        if (cpu) total_cs += cpu->sched_stats.context_switches;
    }
    si->ctx_switches = total_cs;

    return sizeof(SysInfo);
}