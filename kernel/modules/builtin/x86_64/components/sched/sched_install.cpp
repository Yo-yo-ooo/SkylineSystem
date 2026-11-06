//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * sched_install.cpp —— 内建组件 kernel.sched.install (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：
 *   Schedule::Install();                     // 原 line 263
 *   extern void sched_calibrate_tsc(void);
 *   sched_calibrate_tsc();                   // 原 line 264-265
 *
 * 依赖：sched.core（必须已 Init/InitCPUThread）、dev.core（旧 init.cpp 的
 * Install 在 Dev::Init/InitFFMAN 之后）、fs.rootfs（旧顺序 Install 在根文件
 * 系统挂载之后，Lead 确认为避免并行分支乱序显式加此依赖）。
 *
 * 注意：Tier0 保留的 syscall_init/自检线程/sti+IPI+idle 不在此组件内；
 * Lead 已按 B1 报告重写 x86_64_init，组件图在根文件系统与 dev 子系统之后
 * 才把调度器标记为 installed。
 *
 * 导出契约 kernel.sched.installed.v1（v1 内联固定布局）：is_installed()。
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <arch/x86_64/schedule/sched.h>

#define KMOD_CONTRACT_SCHED_INSTALLED_V1 "kernel.sched.installed.v1"
#define KMOD_SCHED_INSTALLED_ABI_V1      1u

/* 定义在 kernel/src/arch/x86_64/schedule/tsc_cal.cpp（旧 init.cpp 内联 extern） */
extern void sched_calibrate_tsc(void);

static int32_t sched_install_init(void);
static void sched_install_exit(void);

KCOMPONENT_BEGIN(sched_install, "kernel.sched.install", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_LOG), 0,
                 "Schedule::Install + TSC calibration (scheduler goes live)",
                 sched_install_init, sched_install_exit, 0, 0, 10000u, 1000u)

KCOMPONENT_IMPORT_FN(sched_install, kprintf_, int, (const char *, ...),
                     "kernel", "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(sched_install, "kernel.sched.core", sched_core)
KCOMPONENT_DEPENDS(sched_install, "kernel.dev.core", dev_core)
/* 旧 init.cpp 顺序：rootfs 挂载 -> ... -> Schedule::Install；
 * 显式依赖避免并行分支把 Install 排到 rootfs 之前。 */
KCOMPONENT_DEPENDS(sched_install, "kernel.fs.rootfs", fs_rootfs)

namespace {

typedef struct kmod_sched_installed_ops {
    uint32_t abi_version;   /* = KMOD_SCHED_INSTALLED_ABI_V1 */
    uint32_t ops_size;      /* = sizeof(kmod_sched_installed_ops_t) */
    int32_t (*is_installed)(void);
} kmod_sched_installed_ops_t;

bool g_sched_installed = false;

int32_t sched_is_installed(void)
{
    return g_sched_installed ? 1 : 0;
}

const kmod_sched_installed_ops_t g_sched_installed_ops = {
    KMOD_SCHED_INSTALLED_ABI_V1,
    (uint32_t)sizeof(kmod_sched_installed_ops_t),
    sched_is_installed,
};

} /* namespace */

static int32_t sched_install_init(void)
{
    Schedule::Install();
    sched_calibrate_tsc();
    g_sched_installed = true;
    kprintf_("[comp.sched.install] init: scheduler installed, TSC calibrated\n");
    return KMOD_OK;
}

static void sched_install_exit(void)
{
    g_sched_installed = false;
    kprintf_("[comp.sched.install] exit (install 不可逆；仅清审计标志)\n");
}

KCOMPONENT_EXPORT_CONTRACT(sched_install, KMOD_CONTRACT_SCHED_INSTALLED_V1,
                           g_sched_installed_ops, KMOD_SCHED_INSTALLED_ABI_V1)
KCOMPONENT_END(sched_install)
