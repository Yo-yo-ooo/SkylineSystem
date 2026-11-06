//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * alloc_policy.cpp —— 内建组件 kernel.alloc.policy (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * B6：分配器策略座位的"内核默认实现提供者"。分层见 kmod_seats.h 与
 * kernel/include/kmod/contracts/alloc.h：
 *   - Tier0 机制（heap.cpp/SLAB/SLUB/PMM/VMM，不可替换）：分配核心、堆对象
 *     布局、失败语义（失败返回 NULL，不 panic）；
 *   - 本组件（Tier1 策略）：把默认策略的**身份与 ops 表**登记为座位的
 *     kernel 默认实现（provider 仍为 -1），不占用座位、不阻断 .ko 接管。
 *
 * 与 B5 sched_policy 相同取舍：不使用 KCOMPONENT_EXPORT_SEAT——否则组件在
 * KmodBootComponents() 提交时会成为座位模块提供者，导致
 * KmodLoadVersion("vendor.demo.alloc", ...) 因"另一模块已占用座位"返回
 * BUSY，与 selftest（demo 加载->热替换->卸载回退内核默认）冲突。
 * 这里用 KmodSeatRegisterKernel（幂等更新 kernel_ops）+ CONTRACT 导出：
 * 默认实现可审计、可观测，模块提供者名额留给可加载 .ko。
 *
 * 默认行为：本组件 ops 的两个策略回调均为 NULL —— heap.cpp 的冷路径 hook
 * 在回调为 NULL 时完全不改变原逻辑，**kmalloc 语义/失败语义逐位不变**。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/kmod_internal.h>   /* KmodSeatRegisterKernel */
#include <kmod/kmod_seats.h>      /* kmod_seat_alloc_policy_ops_t */
#include <kmod/contracts/alloc.h>

static int32_t alloc_policy_init(void);
static void alloc_policy_exit(void);

KCOMPONENT_BEGIN(alloc_policy, "kernel.alloc.policy", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT,
                 (uint64_t)(KMOD_CAP_ALLOC_POLICY | KMOD_CAP_LOG), 0,
                 "default SLAB/SLUB allocator policy (kernel seat default; overrides=NULL)",
                 alloc_policy_init, alloc_policy_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(alloc_policy, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")

namespace {

/* 默认策略 ops：identity/policy_version 供审计与 selftest 识别；
 * 策略回调全 NULL = "完全沿用内核原逻辑"（行为不变）。 */
const kmod_seat_alloc_policy_ops_t g_default_alloc_policy_ops = {
    KMOD_SEAT_ALLOC_POLICY_ABI,
    (uint32_t)sizeof(kmod_seat_alloc_policy_ops_t),
    0u,                 /* policy_version: 0 = 内核默认 */
    0u,                 /* reserved */
    "kernel.heap.default",
    nullptr,            /* large_alloc_threshold: NULL = 默认阈值 */
    nullptr,            /* oom_policy          : NULL = 默认 OOM（返回 NULL） */
};

} /* namespace */

static int32_t alloc_policy_init(void)
{
    /* 幂等更新座位 kernel_ops（provider 保持 -1；不占模块提供者名额）。 */
    int32_t rc = KmodSeatRegisterKernel(
        KMOD_SEAT_ALLOC_POLICY, KMOD_SEAT_ALLOC_POLICY_ABI,
        &g_default_alloc_policy_ops,
        (uint32_t)sizeof(g_default_alloc_policy_ops), KMOD_CAP_SEAT_PROVIDER);
    if (rc != KMOD_OK) {
        kprintf_("[comp.alloc.policy] init FAILED: register kernel default "
                 "rc=%d\n", (int)rc);
        return rc;
    }
    kprintf_("[comp.alloc.policy] init: kernel default heap policy installed "
             "(identity=kernel.heap.default, provider=-1, overrides=NULL)\n");
    return KMOD_OK;
}

static void alloc_policy_exit(void)
{
    /* 座位注册表归 Tier0：组件退出不注销 kernel_ops，避免把分配器打到
     * "无默认策略"状态。热替换由 .ko 提供者覆盖/回退。 */
    kprintf_("[comp.alloc.policy] exit (kernel default ops kept in seat)\n");
}

/* 审计契约导出（KMOD_SYM_CONTRACT，不占座位）：name=契约名，addr=ops 表。 */
KCOMPONENT_EXPORT_CONTRACT(alloc_policy, KMOD_CONTRACT_ALLOC_POLICY_V1,
                           g_default_alloc_policy_ops,
                           KMOD_SEAT_ALLOC_POLICY_ABI)
KCOMPONENT_END(alloc_policy)
