//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * syscall_random.cpp —— 内建组件 kernel.syscall.random：随机数类实现
 * ============================================================================
 * 家族槽位（固定编号）：
 *   20 getrandom -> sys_getrandom
 * 注意：随机数**来源**是可插拔的 Tier1 座位 (KMOD_SEAT_RANDOM)，
 *   sys_getrandom 内部走 KmodSeatRandomFill；本组件只负责固定 syscall
 *   入口的实现注册，不改变座位治理。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <arch/x86_64/schedule/syscall.h>

KCOMPONENT_BEGIN(syscall_random, "kernel.syscall.random", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT, (uint64_t)KMOD_CAP_LOG, 0,
                 "syscall random family (20): getrandom (seat-backed)",
                 nullptr, nullptr, nullptr, nullptr, 0u, 0u)

KCOMPONENT_DEPENDS(syscall_random, "kernel.sched.core", sched)

KCOMPONENT_EXPORT_SYSCALL(syscall_random, 20u, sys_getrandom)

KCOMPONENT_END(syscall_random)
