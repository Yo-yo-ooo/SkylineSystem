//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * sched_core.cpp —— 内建组件 kernel.sched.core (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：
 *   InitFunc("Schedule", Schedule::Init());   // 原 line 209
 *   InitCPUThread();                          // 原 line 210
 *
 * 依赖：arch.smp（CPU 列表/AP 在线）、time.pit/time.rtc（时钟与唤醒源）。
 * 注意 Tier0 保留：syscall_init() 仍在 init.cpp 内联（固定入口/治理表），
 * 旧顺序在 Schedule 之后、Kmod 之前 —— Lead 改 init.cpp 时保持该相对顺序。
 *
 * 导出契约 kernel.sched.core.v1（v1 内联固定布局）：is_ready()。
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/smp/smp.h>   /* InitCPUThread */

#define KMOD_CONTRACT_SCHED_CORE_V1 "kernel.sched.core.v1"
#define KMOD_SCHED_CORE_ABI_V1      1u

static int32_t sched_core_init(void);
static void sched_core_exit(void);

KCOMPONENT_BEGIN(sched_core, "kernel.sched.core", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_MEM_ALLOC | KMOD_CAP_LOG), 0,
                 "scheduler core: Schedule::Init + InitCPUThread (init thread)",
                 sched_core_init, sched_core_exit, 0, 0, 10000u, 1000u)

KCOMPONENT_IMPORT_FN(sched_core, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(sched_core, "kernel.arch.smp", smp)
KCOMPONENT_DEPENDS(sched_core, "kernel.time.pit", pit)
KCOMPONENT_DEPENDS(sched_core, "kernel.time.rtc", rtc)

namespace {

typedef struct kmod_sched_core_ops {
    uint32_t abi_version;   /* = KMOD_SCHED_CORE_ABI_V1 */
    uint32_t ops_size;      /* = sizeof(kmod_sched_core_ops_t) */
    int32_t (*is_ready)(void);
} kmod_sched_core_ops_t;

bool g_sched_ready = false;

int32_t sched_is_ready(void)
{
    return g_sched_ready ? 1 : 0;
}

const kmod_sched_core_ops_t g_sched_core_ops = {
    KMOD_SCHED_CORE_ABI_V1,
    (uint32_t)sizeof(kmod_sched_core_ops_t),
    sched_is_ready,
};

} /* namespace */

static int32_t sched_core_init(void)
{
    Schedule::Init();
    InitCPUThread();
    g_sched_ready = true;
    kprintf_("[comp.sched.core] init: scheduler core + init kernel thread ready\n");
    return KMOD_OK;
}

static void sched_core_exit(void)
{
    g_sched_ready = false;
    kprintf_("[comp.sched.core] exit (scheduler core is Tier0-owned; no teardown)\n");
}

KCOMPONENT_EXPORT_CONTRACT(sched_core, KMOD_CONTRACT_SCHED_CORE_V1,
                           g_sched_core_ops, KMOD_SCHED_CORE_ABI_V1)
KCOMPONENT_END(sched_core)
