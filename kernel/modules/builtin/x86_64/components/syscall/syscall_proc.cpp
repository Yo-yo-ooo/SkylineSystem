//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * syscall_proc.cpp —— 内建组件 kernel.syscall.proc：进程/线程/调度类实现
 * ============================================================================
 * 家族槽位（固定编号）：
 *    6 thread_launch -> sys_thread_launch
 *    7 gettid        -> sys_gettid
 *    8 getpid        -> sys_getpid
 *    9 exit          -> sys_exit        (不返回；治理层对该槽不做 epoch 包裹)
 *   11 yield         -> sched_yield
 *   12 load          -> sys_load
 *   13 launch        -> sys_launch
 *   19 kill          -> sys_kill
 *   31 child_event   -> sys_child_event (子进程退出事件, ABI v1)
 * 16 sysinfo 属 info 家族；10 pmmap 属 mem 家族。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <arch/x86_64/schedule/syscall.h>

KCOMPONENT_BEGIN(syscall_proc, "kernel.syscall.proc", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_MEM_ALLOC | KMOD_CAP_LOG), 0,
                 "syscall proc family (6-9,11-13,19,31): pid/exit/child_event",
                 nullptr, nullptr, nullptr, nullptr, 0u, 0u)

KCOMPONENT_DEPENDS(syscall_proc, "kernel.sched.core", sched)

KCOMPONENT_EXPORT_SYSCALL(syscall_proc, 6u, sys_thread_launch)
KCOMPONENT_EXPORT_SYSCALL(syscall_proc, 7u, sys_gettid)
KCOMPONENT_EXPORT_SYSCALL(syscall_proc, 8u, sys_getpid)
KCOMPONENT_EXPORT_SYSCALL(syscall_proc, 9u, sys_exit)
KCOMPONENT_EXPORT_SYSCALL(syscall_proc, 11u, sched_yield)
KCOMPONENT_EXPORT_SYSCALL(syscall_proc, 12u, sys_load)
KCOMPONENT_EXPORT_SYSCALL(syscall_proc, 13u, sys_launch)
KCOMPONENT_EXPORT_SYSCALL(syscall_proc, 19u, sys_kill)
/* 子进程退出事件（固定槽 31）：desktop 每帧弹出 {pid, exit_code} 立即回收
   窗口，替代 500ms sys_kill(pid,0) 轮询；ABI 见
   kernel/include/arch/x86_64/schedule/syscall.h + 用户 syscalln.h。 */
KCOMPONENT_EXPORT_SYSCALL(syscall_proc, 31u, sys_child_event)

KCOMPONENT_END(syscall_proc)
