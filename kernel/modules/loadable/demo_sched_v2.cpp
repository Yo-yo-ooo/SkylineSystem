//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_sched_v2.cpp —— 示例 .ko：调度策略座位提供者 v2（B5 热替换目标）
 * ----------------------------------------------------------------------------
 * 与 v1 同名 vendor.demo.sched / 2.0.0：policy_version=2、
 * identity="vendor.demo.sched.v2"；覆盖回调仍返回 0 = 沿用内核默认，
 * 因此热替换只改变"策略身份/可读标志"，**不改变调度行为**（无桌面回归）。
 * 用于 selftest：加载 v1 -> KmodHotReplace(v2) -> 读 KmodSeatCurrent 得
 * policy_version==2 -> 失败版本 9.9.9 回退仍 v2 -> 卸载回退内核默认(v0)。
 * ==========================================================================*/
#include <kmod/kmod_module.h>
#include <kmod/kmod_seats.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

static uint64_t demo_sched_base_quantum_ms(const kmod_sched_policy_ctx_t *ctx)
{
    (void)ctx;
    return 0u;
}

static uint64_t demo_sched_quantum_ms(const kmod_sched_policy_ctx_t *ctx)
{
    (void)ctx;
    return 0u;
}

static uint32_t demo_sched_weight_for_priority(uint32_t priority)
{
    (void)priority;
    return 0u;
}

static kmod_seat_sched_policy_ops_t g_sched_policy_ops = {
    KMOD_SEAT_SCHED_POLICY_ABI,
    (uint32_t)sizeof(kmod_seat_sched_policy_ops_t),
    2u,                          /* policy_version = 2 */
    0u,                          /* reserved */
    "vendor.demo.sched.v2",      /* identity（selftest 读） */
    demo_sched_base_quantum_ms,
    demo_sched_quantum_ms,
    demo_sched_weight_for_priority,
};

static int32_t demo_sched_init(void);
static void demo_sched_exit(void);

static int32_t demo_sched_init(void)
{
    kprintf_("[demo.sched] v2 active: policy_version=2 (overrides=default)\n");
    return KMOD_OK;
}

static void demo_sched_exit(void)
{
    kprintf_("[demo.sched] v2 exit -> kernel default policy\n");
}

KMOD_EXPORT_SEAT(KMOD_SEAT_SCHED_POLICY, g_sched_policy_ops,
                 KMOD_SEAT_SCHED_POLICY_ABI)

KMOD_MANIFEST("vendor.demo.sched", "2.0.0", KMOD_TIER_POLICY,
              KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE,
              (uint64_t)(KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                         KMOD_CAP_LOG),
              demo_sched_init, demo_sched_exit);
