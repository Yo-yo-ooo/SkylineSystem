//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_alloc_v1.cpp —— 示例 .ko：分配器策略座位提供者 v1（B6）
 * ----------------------------------------------------------------------------
 * 名称 vendor.demo.alloc / 1.0.0 / TIER_POLICY
 * flags: KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE
 * caps : SEAT_PROVIDER | HOT_REPLACE | LOG
 *
 * 导出 KMOD_SEAT_ALLOC_POLICY ("kernel.seat.alloc.policy.v1") 实现：
 *   - policy_version = 1，identity = "vendor.demo.alloc.v1"（selftest 可读）；
 *   - 两个冷路径回调均返回 0 = "沿用内核默认"：large 阈值取默认、OOM 不重试
 *     （默认失败返回 NULL）——不改变 kmalloc 语义/失败语义/性能热路径。
 *
 * 治理链路：加载 -> KmodSeatAttachModule 安装 module_ops（provider=本模块）；
 *   KmodHotReplace 到 v2 -> 座位原子切到新 ops；卸载 -> KmodSeatDetachModule
 *   回退 Tier0 内核默认（policy_version=0 / identity=kernel.heap.default）。
 * ==========================================================================*/
#include <kmod/kmod_module.h>
#include <kmod/kmod_seats.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* 冷路径策略回调：返回 0 = 沿用内核默认（见 kmod_seats.h 语义）。 */
static uint64_t demo_alloc_large_threshold(uint64_t request,
                                           uint64_t default_threshold)
{
    (void)request;
    (void)default_threshold;
    return 0u;                    /* 0 = 默认阈值，不改变路径 */
}

static int32_t demo_alloc_oom_policy(uint64_t request, uint32_t kind)
{
    (void)request;
    (void)kind;
    return KMOD_ALLOC_OOM_DEFAULT; /* 0 = 默认：失败即返回 NULL，不重试 */
}

static kmod_seat_alloc_policy_ops_t g_alloc_policy_ops = {
    KMOD_SEAT_ALLOC_POLICY_ABI,
    (uint32_t)sizeof(kmod_seat_alloc_policy_ops_t),
    1u,                          /* policy_version = 1 */
    0u,                          /* reserved */
    "vendor.demo.alloc.v1",      /* identity（selftest 读） */
    demo_alloc_large_threshold,
    demo_alloc_oom_policy,
};

static int32_t demo_alloc_init(void);
static void demo_alloc_exit(void);

static int32_t demo_alloc_init(void)
{
    kprintf_("[demo.alloc] v1 active: policy_version=1 (overrides=default)\n");
    return KMOD_OK;
}

static void demo_alloc_exit(void)
{
    kprintf_("[demo.alloc] v1 exit -> kernel default heap policy\n");
}

KMOD_EXPORT_SEAT(KMOD_SEAT_ALLOC_POLICY, g_alloc_policy_ops,
                 KMOD_SEAT_ALLOC_POLICY_ABI)

KMOD_MANIFEST("vendor.demo.alloc", "1.0.0", KMOD_TIER_POLICY,
              KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE,
              (uint64_t)(KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                         KMOD_CAP_LOG),
              demo_alloc_init, demo_alloc_exit);
