//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#ifndef _SYSINFO_H_
#define _SYSINFO_H_

#include <stdint.h>
#include <pdef.h>

#define SYSINFO_MAGIC   0x49535953ULL     /* 'SYSI' LE */
#define SYSINFO_ABI_VER     1                

PACK(typedef struct SysInfo {

    /* ---- 身份 (offset 0) ---- */
    uint64_t magic;              /* SYSINFO_MAGIC — 结构没填错位置的凭证 */
    uint32_t abi_version;        /* SYSINFO_ABI */
    uint32_t kernel_version;     /* KERNEL_VERSION: major<<16|minor<<8|patch */

    /* ---- CPU (offset 16) ---- */
    uint32_t ncpus;              /* 在线核心数 (smp_last_cpu+1) */
    uint32_t cpu_family;         /* CPUID.1.EAX 打包: fam_ext<<20|fam<<16|ext_model<<12|model<<4|stepping */
    uint32_t cpu_features_edx;   /* CPUID.1.EDX — fpu/tsc/msr/apic... */
    uint32_t cpu_features_ecx;   /* CPUID.1.ECX — sse4_2/avx/rdrand...  */
    char     cpu_brand[48];      /* CPUID.0x80000002~4 品牌串, 零结尾 */

    
    uint32_t simd_mask[MAX_CPU];   /* 每核 xsave 掩码, 未用核=0 */

    
    uint64_t mem_total;          /* 物理总量 = pmm_bitmap_pages << 12 */
    uint64_t mem_free;           /* O(1) = PMM::FreePages() << 12 */
    uint64_t mem_used;           /* total - free (PCP 缓存页含在 used) */

    
    uint64_t uptime_ms;          /* PIT::TimeSinceBootMS() */
    uint64_t nprocs;             /* 存活进程数 (pid2proc_tree->size) */
    uint64_t nthreads_approx;    /* ≈ sched_tid 累计分配 — 近似, 注记 */
    uint64_t ctx_switches;       /* Σ per-CPU sched_stats */

    
    uint64_t reserved[8];        /* 恒 0; ABI 扩张空间 */

}) SysInfo;


#endif /* _SYSINFO_H_ */