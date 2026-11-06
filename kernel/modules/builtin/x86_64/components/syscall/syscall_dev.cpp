//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * syscall_dev.cpp —— 内建组件 kernel.syscall.dev：设备/调试类实现
 * ============================================================================
 * 家族槽位（固定编号）：
 *   21 dev_mmap -> sys_dev_mmap   (设备 MMIO 映射)
 *   24 dbgsout  -> sys_dbgsout    (用户态调试输出)
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <arch/x86_64/schedule/syscall.h>

KCOMPONENT_BEGIN(syscall_dev, "kernel.syscall.dev", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_DEVICE | KMOD_CAP_MMIO | KMOD_CAP_LOG), 0,
                 "syscall dev family (21,24): dev_mmap/debug output",
                 nullptr, nullptr, nullptr, nullptr, 0u, 0u)

KCOMPONENT_DEPENDS(syscall_dev, "kernel.sched.core", sched)
KCOMPONENT_DEPENDS(syscall_dev, "kernel.dev.core", dev)

KCOMPONENT_EXPORT_SYSCALL(syscall_dev, 21u, sys_dev_mmap)
KCOMPONENT_EXPORT_SYSCALL(syscall_dev, 24u, sys_dbgsout)

KCOMPONENT_END(syscall_dev)
