//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * syscall_mem.cpp —— 内建组件 kernel.syscall.mem：内存/映射类实现
 * ============================================================================
 * 家族槽位（固定编号）：
 *   10 pmmap      -> sys_pmmapSHARE
 *   14 mmap       -> sys_mmap
 *   15 munmap     -> sys_munmap
 *   18 arch_prctl -> sys_arch_prctl
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <arch/x86_64/schedule/syscall.h>

KCOMPONENT_BEGIN(syscall_mem, "kernel.syscall.mem", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_MEM_ALLOC | KMOD_CAP_MEM_DMA), 0,
                 "syscall mem family (10,14,15,18): pmmap/mmap/munmap/prctl",
                 nullptr, nullptr, nullptr, nullptr, 0u, 0u)

KCOMPONENT_DEPENDS(syscall_mem, "kernel.sched.core", sched)

KCOMPONENT_EXPORT_SYSCALL(syscall_mem, 10u, sys_pmmapSHARE)
KCOMPONENT_EXPORT_SYSCALL(syscall_mem, 14u, sys_mmap)
KCOMPONENT_EXPORT_SYSCALL(syscall_mem, 15u, sys_munmap)
KCOMPONENT_EXPORT_SYSCALL(syscall_mem, 18u, sys_arch_prctl)

KCOMPONENT_END(syscall_mem)
