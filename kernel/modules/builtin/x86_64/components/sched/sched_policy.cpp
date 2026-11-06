//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * sched_policy.cpp —— 内建组件 kernel.sched.policy.eevdf (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * B5：调度策略座位的"内核默认实现提供者"。分层见 kmod_seats.h 与
 * kernel/include/kmod/contracts/sched.h：
 *   - Tier0 机制（sched.cpp，不可替换）：队列/切换/时钟/EEVDF 记账/quantum
 *     默认算法；
 *   - 本组件（Tier1 策略）：把默认策略的**身份与 ops 表**登记为座位的
 *     kernel 默认实现（provider 仍为 -1），不占用座位、不阻断 .ko 接管。
 *
 * 为什么不用 KCOMPONENT_EXPORT_SEAT：
 *   导出 KMOD_SYM_SEAT 会让组件在 KmodBootComponents() 提交阶段成为座位
 *   **模块提供者**（provider=模块下标），此后 KmodLoad("vendor.demo.sched")
 *   会因"另一模块已占用座位"返回 KMOD_ERR_BUSY，与 selftest 期望
 *   （demo 加载->热替换->卸载回退内核默认）冲突。因此这里采用
 *   KmodSeatRegisterKernel(幂等更新 kernel_ops) + CONTRACT 导出的组合：
 *   默认实现可审计、可观测，座位的模块提供者名额留给可加载 .ko。
 *
 * 默认行为：本组件 ops 的所有覆盖回调为 NULL —— sched.cpp 的 wrapper 在
 * 回调为 NULL 时直接返回原 EEVDF/反馈逻辑结果，**默认调度逐位不变**。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/kmod_internal.h>   /* KmodSeatRegisterKernel */
#include <kmod/kmod_seats.h>      /* kmod_seat_sched_policy_ops_t */
#include <kmod/contracts/sched.h>

#define KMOD_COMP_SCHED_POLICY_ID "kernel.sched.policy.eevdf"

static int32_t sched_policy_init(void);
static void sched_policy_exit(void);

KCOMPONENT_BEGIN(sched_policy, "kernel.sched.policy.eevdf", "1.0.0",
                 KMOD_TIER_POLICY, KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_SEAT_PROVIDER | KMOD_CAP_LOG), 0,
                 "default EEVDF/feedback scheduling policy provider (kernel seat default)",
                 sched_policy_init, sched_policy_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(sched_policy, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(sched_policy, "kernel.sched.core", sched_core)

namespace {

/* 默认策略 ops：identity/policy_version 供审计与 selftest 识别；
 * 覆盖回调全 NULL = "完全沿用内核 EEVDF/反馈逻辑"（行为不变）。 */
const kmod_seat_sched_policy_ops_t g_default_sched_policy_ops = {
    KMOD_SEAT_SCHED_POLICY_ABI,
    (uint32_t)sizeof(kmod_seat_sched_policy_ops_t),
    0u,             /* policy_version: 0 = 内核默认 */
    0u,             /* reserved */
    "kernel.eevdf", /* identity */
    nullptr,        /* base_quantum_ms  : NULL = 默认 */
    nullptr,        /* quantum_ms       : NULL = 默认 */
    nullptr,        /* weight_for_priority: NULL = 默认 */
};

} /* namespace */

static int32_t sched_policy_init(void)
{
    /* 幂等更新座位 kernel_ops（provider 保持 -1；不占用模块提供者名额）。 */
    int32_t rc = KmodSeatRegisterKernel(
        KMOD_SEAT_SCHED_POLICY, KMOD_SEAT_SCHED_POLICY_ABI,
        &g_default_sched_policy_ops,
        (uint32_t)sizeof(g_default_sched_policy_ops), KMOD_CAP_SEAT_PROVIDER);
    if (rc != KMOD_OK) {
        kprintf_("[comp.sched.policy] init FAILED: register kernel default "
                 "rc=%d\n", (int)rc);
        return rc;
    }
    kprintf_("[comp.sched.policy] init: kernel default EEVDF policy installed "
             "(identity=kernel.eevdf, provider=-1, overrides=NULL)\n");
    return KMOD_OK;
}

static void sched_policy_exit(void)
{
    /* 座位注册表归 Tier0 所有：组件退出不注销 kernel_ops，避免把调度器
     * 打到"无默认策略"状态。热替换场景由 .ko 提供者覆盖/回退。 */
    kprintf_("[comp.sched.policy] exit (kernel default ops kept in seat)\n");
}

/* 审计契约导出（KMOD_SYM_CONTRACT，不占座位）：name=契约名，addr=ops 表。
 * 供 KMOD-SELFTEST / 诊断侧机械校验"默认策略实现存在且 ABI 一致"。 */
KCOMPONENT_EXPORT_CONTRACT(sched_policy, KMOD_CONTRACT_SCHED_POLICY_V1,
                           g_default_sched_policy_ops,
                           KMOD_SEAT_SCHED_POLICY_ABI)
KCOMPONENT_END(sched_policy)
