//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#ifndef _SYSINFO_H_
#define _SYSINFO_H_

#include <stdint.h>
#include <stddef.h>

/* ============================================================
 * SysInfo — 实时共享只读信息页 (多页, 按核数伸缩)
 *
 * 协议:
 *   内核 = 唯一写者 (init 填静态, idle 线程刷动态)
 *   用户 = 只读者 (PTE 无 W 位, 写入 #PF — 硬件级只读)
 *   页数 = occupy_pages — 用户映射前就能从头知道
 *   布局: 头不 packed (FAM 偏移 = sizeof 可靠),
 *         ncpus 是 simd_mask[] 长度权威
 *
 *   映射长度 = occupy_pages * 4096
 *   (物理帧不必连续 — 内核逐页穿透, 用户看到的是连续 VA)
 * ============================================================ */

#define SYSINFO_MAGIC     0x49535953ULL      /* 'SYSI' LE */
#define SYSINFO_ABI_VER  0                  // In Test Version (ABI VER=0)

typedef struct SysInfo {

    /* ---- 映射元信息 (静态, 用户第一个读) ---- */
    uint64_t occupy_pages;        /*  本结构映射占用的总页数 —
                                     用户侧 munmap/遍历都以此为准 */

    /* ---- 身份 (静态) ---- */
    uint64_t magic;
    uint32_t abi_version;
    uint32_t kernel_version;     /* major<<16|minor<<8|patch, v1=0 */

    /* ---- CPU (静态) ---- */
    uint32_t ncpus;              /*  simd_mask[] 长度权威 */
    uint32_t cpu_family;
    uint32_t cpu_features_edx;
    uint32_t cpu_features_ecx;   /* 用户态 SIMD 分派依据 */
    char     cpu_brand[48];

    /* ---- 内存 (idle 刷新) ---- */
    uint64_t mem_total;
    uint64_t mem_free;           /* PMM O(1) 计数器; PCP 页含在 used */
    uint64_t mem_used;

    /* ---- 运行时 (idle 刷新) ---- */
    uint64_t uptime_ms;
    uint64_t nprocs;
    uint64_t nthreads_approx;    /* ≈ sched_tid, 含已退出 — 近似 */
    uint64_t ctx_switches;       /* Σ per-CPU */

    /* ---- 预留 (恒 0, ABI 扩张) ---- */
    uint64_t reserved[8];

    /* ---- 尾随: 每核 SIMD 掩码 (静态), 有效长度 = ncpus ---- */
    uint32_t simd_mask[];

} SysInfo;

/* 完整数据大小 (头 + n 个核) */
#define SYSINFO_SIZE(ncpus) \
    (sizeof(SysInfo) + (size_t)(ncpus) * sizeof(uint32_t))

/* n 个核需要几页 */
#define SYSINFO_PAGES(ncpus) \
    ((SYSINFO_SIZE(ncpus) + 4095) / 4096)

/* 满配页数 — 内核帧数组按此定长 */
#define SYSINFO_MAX_PAGES \
    ((SYSINFO_SIZE(MAX_CPU) + 4095) / 4096)

#endif /* _SYSINFO_H_ */