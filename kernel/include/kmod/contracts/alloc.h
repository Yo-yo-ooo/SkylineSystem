//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/alloc.h —— 分配器策略私有契约（v1）
 * ============================================================================
 * 两层模型（原则 5：开放策略，不开放核心机制）：
 *   Tier0 机制（固定，不可替换）：PMM/VMM 页表机制、SLAB/SLUB 分配核心、
 *     内核堆对象布局、失败语义（返回 NULL 而非 panic）。
 *   Tier1 策略（座位 kernel.seat.alloc.policy.v1，可替换）：只提供**冷路径
 *     参数**——large-allocation 阈值与 OOM 决策；回调返回 0/NULL 表示
 *     "完全沿用内核默认"，因此不改变 kmalloc 语义/失败语义/默认 OOM 行为。
 *
 * 性能约束（必须遵守）：策略查询只允许出现在
 *   - large path：request >= KMOD_ALLOC_LARGE_MIN(4 KiB)；
 *   - 分配失败后的 OOM/回退路径。
 *   小对象（< 4 KiB）热路径**禁止**查询座位，零额外开销。
 *
 * 座位 ops 结构定义在 <kmod/kmod_seats.h>；本头文件定义策略常量和语义。
 * 回调运行在分配器上下文：不得再分配内核堆（禁止重入 kmalloc/kfree）、
 * 不得睡眠/阻塞/打印（panic 路径可能重入）；应 O(1) 且无锁竞争。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACT_ALLOC_H_
#define _KMOD_CONTRACT_ALLOC_H_

#include <stdint.h>
#include <stddef.h>
#include <kmod/kmod_abi.h>   /* KMOD_ABI_STATIC_ASSERT */

/* 审计用私有契约名：内建默认策略组件导出同名 contract（不占座位）。 */
#define KMOD_CONTRACT_ALLOC_POLICY_V1 "kernel.alloc.policy.v1"

/* large path 门限：只有 >= 此值的请求才允许查询策略（与 heap.cpp 注释一致）。 */
#define KMOD_ALLOC_LARGE_MIN            4096ull
/* 内核默认 large 阈值（heap.cpp 传入 default_threshold 的基准值）。 */
#define KMOD_ALLOC_LARGE_DEFAULT_THRESHOLD 4096ull

/* oom_policy 的 kind 入参。 */
#define KMOD_ALLOC_OOM_KIND_KMALLOC     1u  /* kmalloc/kcalloc/krealloc 增长失败 */
#define KMOD_ALLOC_OOM_KIND_ALIGNED     2u  /* kmalloc_aligned 失败（预留） */
#define KMOD_ALLOC_OOM_KIND_LARGE       3u  /* 不可满足的超大请求（预留） */

/* oom_policy 返回值语义（严格）： */
#define KMOD_ALLOC_OOM_DEFAULT          0   /* 沿用默认：返回 NULL（不重试） */
#define KMOD_ALLOC_OOM_RETRY            1   /* 允许同路径重试一次（仍失败则 NULL） */
/* 其它正整数 = 策略拒绝：直接返回 NULL（与默认结果相同，仅审计语义不同）；
 * 负值保留（当前视为默认）。 */

#endif /* _KMOD_CONTRACT_ALLOC_H_ */
