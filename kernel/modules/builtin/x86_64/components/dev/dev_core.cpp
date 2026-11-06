//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * dev_core.cpp —— 内建组件 kernel.dev.core (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：
 *   InitFunc("VsDev", Dev::Init());           // 原 line 225
 *   InitFunc("File & MP MAN", InitFFMAN());   // 原 line 226
 *
 * 依赖 sched.core：Dev::Init 的 hashmap / InitFFMAN 的挂载点表在旧顺序里位于
 * Schedule 之后（原 line 209/210 -> 225/226）；B3/B4 的驱动与 VFS 组件都依赖
 * 本组件提供 Dev 注册表与挂载点表。
 *
 * 导出契约 kernel.dev.v1（v1 内联固定布局）：is_ready()。
 * ==========================================================================*/
#include <kmod/kmod_component.h>

#include <drivers/dev/dev.h>
#include <fs/fd.h>   /* InitFFMAN */

#define KMOD_CONTRACT_DEV_V1 "kernel.dev.v1"
#define KMOD_DEV_ABI_V1      1u

static int32_t dev_core_init(void);
static void dev_core_exit(void);

KCOMPONENT_BEGIN(dev_core, "kernel.dev.core", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_MEM_ALLOC | KMOD_CAP_DEVICE |
                                          KMOD_CAP_LOG),
                 0, "Dev registry + file/mount manager (Dev::Init + InitFFMAN)",
                 dev_core_init, dev_core_exit, 0, 0, 10000u, 1000u)

KCOMPONENT_IMPORT_FN(dev_core, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(dev_core, "kernel.sched.core", sched_core)

namespace {

typedef struct kmod_dev_ops {
    uint32_t abi_version;   /* = KMOD_DEV_ABI_V1 */
    uint32_t ops_size;      /* = sizeof(kmod_dev_ops_t) */
    int32_t (*is_ready)(void);
} kmod_dev_ops_t;

bool g_dev_ready = false;

int32_t dev_is_ready(void)
{
    return g_dev_ready ? 1 : 0;
}

const kmod_dev_ops_t g_dev_ops = {
    KMOD_DEV_ABI_V1,
    (uint32_t)sizeof(kmod_dev_ops_t),
    dev_is_ready,
};

} /* namespace */

static int32_t dev_core_init(void)
{
    Dev::Init();
    InitFFMAN();
    g_dev_ready = true;
    kprintf_("[comp.dev.core] init: Dev registry + file/mount manager ready\n");
    return KMOD_OK;
}

static void dev_core_exit(void)
{
    g_dev_ready = false;
    kprintf_("[comp.dev.core] exit (Dev/MP tables owned by Tier0 subsystems)\n");
}

KCOMPONENT_EXPORT_CONTRACT(dev_core, KMOD_CONTRACT_DEV_V1, g_dev_ops,
                           KMOD_DEV_ABI_V1)
KCOMPONENT_END(dev_core)
