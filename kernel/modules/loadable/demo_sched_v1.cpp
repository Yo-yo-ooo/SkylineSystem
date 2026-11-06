//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * demo_sched_v1.cpp —— 示例 .ko：调度策略座位提供者 v1（B5）
 * ----------------------------------------------------------------------------
 * 名称 vendor.demo.sched / 1.0.0 / TIER_POLICY
 * flags: KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE
 * caps : SEAT_PROVIDER | HOT_REPLACE | LOG
 *
 * 导出 KMOD_SEAT_SCHED_POLICY ("kernel.seat.sched.policy.v1") 实现：
 *   - policy_version = 1，identity = "vendor.demo.sched.v1"（selftest 可读）；
 *   - 覆盖回调全部返回 0 = "沿用内核 EEVDF/反馈默认"，**不改变调度行为**，
 *     只作为"策略提供者"身份参与座位治理/热替换/卸载回退验证。
 *
 * 治理链路：加载 -> KmodSeatAttachModule 安装 module_ops（provider=本模块）；
 *   KmodHotReplace 到 v2 -> 座位原子切到新 ops；卸载 -> KmodSeatDetachModule
 *   回退 Tier0 内核默认（policy_version=0 / identity=kernel.eevdf）。
 * ==========================================================================*/
#include <kmod/kmod_module.h>
#include <kmod/kmod_seats.h>

#include <stdint.h>

KMOD_IMPORT_FN(kprintf_, int, (const char *, ...), "kernel", "kprintf_",
               "kernel.log.v1");

/* 策略覆盖回调：返回 0 表示沿用内核默认（见 kmod_seats.h 语义）。
 * 不改变选线程/quantum 数值 —— 仅为"策略提供者"身份演示。 */
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
    1u,                          /* policy_version = 1 */
    0u,                          /* reserved */
    "vendor.demo.sched.v1",      /* identity（selftest 读） */
    demo_sched_base_quantum_ms,
    demo_sched_quantum_ms,
    demo_sched_weight_for_priority,
};

static int32_t demo_sched_init(void);
static void demo_sched_exit(void);

static int32_t demo_sched_init(void)
{
    kprintf_("[demo.sched] v1 active: policy_version=1 (overrides=default)\n");
    return KMOD_OK;
}

static void demo_sched_exit(void)
{
    kprintf_("[demo.sched] v1 exit -> kernel default policy\n");
}

KMOD_EXPORT_SEAT(KMOD_SEAT_SCHED_POLICY, g_sched_policy_ops,
                 KMOD_SEAT_SCHED_POLICY_ABI)

KMOD_MANIFEST("vendor.demo.sched", "1.0.0", KMOD_TIER_POLICY,
              KMOD_MF_HOTREPLACE | KMOD_MF_UNLOADABLE,
              (uint64_t)(KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_HOT_REPLACE |
                         KMOD_CAP_LOG),
              demo_sched_init, demo_sched_exit);
