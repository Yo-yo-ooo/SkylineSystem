//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * syscall_info.cpp —— 内建组件 kernel.syscall.info：系统/设备信息类实现
 * ============================================================================
 * 家族槽位（固定编号）：
 *   16 sysinfo      -> sys_sysinfo
 *   25 dev_getinfo  -> sys_dev_getinfo
 *   26 dev_ioctl    -> sys_dev_ioctl
 * 29 SYSCALL_KMOD_INFO 是 Tier0 治理自省槽，由 syscall_registry.cpp 内部
 * 分派到 KmodSyscallCopyInfoToUser，*不*走组件。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <arch/x86_64/schedule/syscall.h>

KCOMPONENT_BEGIN(syscall_info, "kernel.syscall.info", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_LOG | KMOD_CAP_DEVICE), 0,
                 "syscall info family (16,25,26): sysinfo/dev_getinfo/dev_ioctl",
                 nullptr, nullptr, nullptr, nullptr, 0u, 0u)

KCOMPONENT_DEPENDS(syscall_info, "kernel.sched.core", sched)
KCOMPONENT_DEPENDS(syscall_info, "kernel.dev.core", dev)

KCOMPONENT_EXPORT_SYSCALL(syscall_info, 16u, sys_sysinfo)
KCOMPONENT_EXPORT_SYSCALL(syscall_info, 25u, sys_dev_getinfo)
KCOMPONENT_EXPORT_SYSCALL(syscall_info, 26u, sys_dev_ioctl)

KCOMPONENT_END(syscall_info)
