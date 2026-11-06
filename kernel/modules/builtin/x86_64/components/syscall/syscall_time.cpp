//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * syscall_time.cpp —— 内建组件 kernel.syscall.time：时间类实现
 * ============================================================================
 * 家族槽位（固定编号）：
 *   17 time         -> sys_time          (RTC 墙钟秒)
 *   27 monotonic_ms -> sys_monotonic_ms  (PIT 毫秒单调时钟)
 *   28 sleep_ms     -> sys_sleep_ms      (真睡眠，走调度器定时器轮)
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <arch/x86_64/schedule/syscall.h>

KCOMPONENT_BEGIN(syscall_time, "kernel.syscall.time", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT, (uint64_t)KMOD_CAP_LOG, 0,
                 "syscall time family (17,27,28): wall/monotonic/sleep",
                 nullptr, nullptr, nullptr, nullptr, 0u, 0u)

KCOMPONENT_DEPENDS(syscall_time, "kernel.sched.core", sched)
KCOMPONENT_DEPENDS(syscall_time, "kernel.time.rtc", rtc)

KCOMPONENT_EXPORT_SYSCALL(syscall_time, 17u, sys_time)
KCOMPONENT_EXPORT_SYSCALL(syscall_time, 27u, sys_monotonic_ms)
KCOMPONENT_EXPORT_SYSCALL(syscall_time, 28u, sys_sleep_ms)

KCOMPONENT_END(syscall_time)
