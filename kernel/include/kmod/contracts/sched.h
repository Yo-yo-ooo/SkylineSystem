//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/sched.h —— 调度策略私有契约（v1）
 * ============================================================================
 * 两层模型（原则 5：开放策略，不开放核心机制）：
 *   Tier0 机制（固定，不可替换）：运行队列、上下文切换、时钟、EEVDF 记账
 *     核心、quantum 默认计算 get_dynamic_quantum/eligibility_capped_quantum。
 *   Tier1 策略（座位 kernel.seat.sched.policy.v1，可替换）：只提供**参数/
 *     决策覆盖**——quantum(ms) 覆盖与 weight(优先级->权重) 覆盖；返回 0 表示
 *     "沿用内核默认"，因此默认路径与未启用策略时逐位一致。
 *
 * 座位 ops 结构定义在 <kmod/kmod_seats.h>（模块与内核共用同一份 ABI）；
 * 本头文件定义策略决策上下文（stage/输入参数/默认值）与审计契约名。
 *
 * 安全性约束：策略回调运行在调度器临界路径（持 sched_lock 或中断上下文），
 * 不得睡眠/阻塞/分配内存/重入调度；内核不解释策略语义，只做 ABI 尺寸、
 * 版本、空指针与数值合理性的机械校验。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACT_SCHED_H_
#define _KMOD_CONTRACT_SCHED_H_

#include <stdint.h>
#include <stddef.h>
#include <kmod/kmod_abi.h>   /* KMOD_ABI_STATIC_ASSERT */

/* 审计用私有契约名：内建默认策略组件导出同名 contract（不占座位），
 * 便于观测/诊断确认"默认策略实现"的身份。 */
#define KMOD_CONTRACT_SCHED_POLICY_V1 "kernel.sched.policy.v1"

/* 策略决策阶段（ctx.stage）。 */
#define KMOD_SCHED_STAGE_BASE        1u  /* base_quantum：定时器重臂/基准片长 */
#define KMOD_SCHED_STAGE_DYNAMIC     2u  /* dynamic_quantum：RIP 反馈融合后片长 */
#define KMOD_SCHED_STAGE_ELIGIBILITY 3u  /* eligibility_capped：EEVDF 预算截断 */

/* 策略决策上下文（v1 冻结；任何字段变更必须升 ABI 并保留旧结构）。
 *
 * 输入：cpu_id / priority / weight / base_quantum_ms 为当前决策点的真实参数；
 *       default_quantum_ms 为**内核默认逻辑算出的值**（策略可参考或忽略）。
 * 输出：策略回调返回值（ms）；0 = 沿用 default_quantum_ms。 */
typedef struct kmod_sched_policy_ctx {
    uint32_t cpu_id;             /* 当前 CPU 逻辑 id */
    uint32_t priority;           /* 线程优先级 0..15（越高越优先） */
    uint32_t stage;              /* KMOD_SCHED_STAGE_* */
    uint32_t reserved;           /* 必须 0 */
    uint64_t base_quantum_ms;    /* 本 CPU 基准片长（ms） */
    uint64_t weight;             /* 线程权重（sched_prio_to_weight 值） */
    uint64_t default_quantum_ms; /* 内核默认逻辑给出的片长（ms） */
} kmod_sched_policy_ctx_t;

KMOD_ABI_STATIC_ASSERT(sizeof(kmod_sched_policy_ctx_t) == 40u,
                       "kmod_sched_policy_ctx_t ABI freeze");

#endif /* _KMOD_CONTRACT_SCHED_H_ */
