//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_alloc_v2.cpp —— 示例 .ko：分配器策略座位提供者 v2（B6 热替换目标）
 * ----------------------------------------------------------------------------
 * 与 v1 同名 vendor.demo.alloc / 2.0.0：policy_version=2、
 * identity="vendor.demo.alloc.v2"；两个冷路径回调仍返回 0 = 沿用内核默认，
 * 因此热替换只改变"策略身份/可读标志"，**不改变 kmalloc 语义/失败语义/
 * 性能路径**（无桌面/DOOM 回归）。
 * 用于 selftest：加载 v1 -> KmodHotReplace(v2) -> 读 KmodSeatCurrent 得
 * policy_version==2 -> 失败版本 9.9.9 回退仍 v2 -> 卸载回退内核默认(v0)。
 * ==========================================================================*/
#include <kmod/kmod_module.h>
#include <kmod/kmod_seats.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

static uint64_t demo_alloc_large_threshold(uint64_t request,
                                           uint64_t default_threshold)
{
    (void)request;
    (void)default_threshold;
    return 0u;
}

static int32_t demo_alloc_oom_policy(uint64_t request, uint32_t kind)
{
    (void)request;
    (void)kind;
    return KMOD_ALLOC_OOM_DEFAULT;
}

static kmod_seat_alloc_policy_ops_t g_alloc_policy_ops = {
    KMOD_SEAT_ALLOC_POLICY_ABI,
    (uint32_t)sizeof(kmod_seat_alloc_policy_ops_t),
    2u,                          /* policy_version = 2 */
    0u,                          /* reserved */
    "vendor.demo.alloc.v2",      /* identity（selftest 读） */
    demo_alloc_large_threshold,
    demo_alloc_oom_policy,
};

static int32_t demo_alloc_init(void);
static void demo_alloc_exit(void);

static int32_t demo_alloc_init(void)
{
    kprintf_("[demo.alloc] v2 active: policy_version=2 (overrides=default)\n");
    return KMOD_OK;
}

static void demo_alloc_exit(void)
{
    kprintf_("[demo.alloc] v2 exit -> kernel default heap policy\n");
}

KMOD_EXPORT_SEAT(KMOD_SEAT_ALLOC_POLICY, g_alloc_policy_ops,
                 KMOD_SEAT_ALLOC_POLICY_ABI)

KMOD_MANIFEST("vendor.demo.alloc", "2.0.0", KMOD_TIER_POLICY,
              KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE,
              (uint64_t)(KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                         KMOD_CAP_LOG),
              demo_alloc_init, demo_alloc_exit);
