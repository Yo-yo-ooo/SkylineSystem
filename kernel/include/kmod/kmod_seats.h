//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kmod_seats.h —— Tier0 内核固定机制的"座位(seat)"契约 ABI
 * ============================================================================
 * 原则 5：开放策略，不开放核心机制。
 *   - 机制（syscall 入口、VFS 契约、调度器核心、随机数系统调用路径等）固定在
 *     Tier 0，代码不可被 .ko 替换；
 *   - 策略/实现通过"座位"由 Tier 1+ 模块提供，内核只做：
 *       座位注册、A BI 尺寸校验、能力治理、原子切换、失败回退。
 *   - 座位 ops 结构由内核定义（这是 Tier0 机制契约，不是模块间业务接口），
 *     模块必须逐字段匹配，内核在加载期校验 sizeof 与 abi_version。
 *
 * 当前实现的结构（v1）：
 *   kmod_seat_random_ops_t      —— 随机数提供者，被 sys_getrandom() 路径消费。
 *     默认提供者 = 内核 Tier0 内建实现；模块可用 KMOD_EXPORT_SEAT() 取代，
 *     内核侧通过 KmodSeatRandomU64()/KmodSeatRandomFill() 原子读取当前提供者。
 *   kmod_seat_sched_policy_ops_t —— 调度策略提供者（B5）。Tier0 固定 EEVDF
 *     记账/队列/切换机制；策略只覆盖 quantum/weight 等参数。内核默认 ops 由
 *     Tier0（sched.cpp）注册，内建组件 kernel.sched.policy.eevdf 会把它升级为
 *     组件内实现；模块可用 KMOD_EXPORT_SEAT() 热替换，卸载后回退内核默认。
 * ==========================================================================*/
#ifndef _KMOD_SEATS_H_
#define _KMOD_SEATS_H_

#include <stdint.h>
#include <kmod/contracts/sched.h>   /* kmod_sched_policy_ctx_t */
#include <kmod/contracts/alloc.h>   /* large/OOM 策略常量 */

#ifdef __cplusplus
extern "C" {
#endif

#define KMOD_SEAT_RANDOM_ABI   1u

/* 随机数座位 ABI v1。语义契约（固定）：
 *   u64()         返回 64 位随机值（每次调用独立、无状态副作用）
 *   fill(buf,len) 用随机字节填满 [buf, buf+len)，可为 NULL（内核退化为循环 u64）
 * 模块实现必须保证：不得睡眠/阻塞、不得重入（由调用方 epoch 治理）、
 * 不得返回固定常量（demo 诊断模块除外，见 docs/kernel-modules.md 回退说明）。 */
typedef struct kmod_seat_random_ops {
    uint32_t abi_version;      /* 必须 == KMOD_SEAT_RANDOM_ABI */
    uint32_t ops_size;         /* 必须 == sizeof(kmod_seat_random_ops_t) */
    uint64_t (*u64)(void);     /* 必须非 NULL */
    void (*fill)(uint8_t *buf, uint64_t len);  /* 可 NULL */
} kmod_seat_random_ops_t;

/* 内核内部调用入口（Tier0 适配层；sys_getrandom 走这里） */
uint64_t KmodSeatRandomU64(void);
void KmodSeatRandomFill(uint8_t *buf, uint64_t len);

/* ------------------------------------------------------------------ */
/* 座位：调度策略 kernel.seat.sched.policy.v1（B5）                    */
/* ------------------------------------------------------------------ */
/* 机制/策略边界（原则 5）：
 *   Tier0 固定：运行队列(rb-tree)/上下文切换/时钟/EEVDF 记账核心，以及
 *                quantum 默认算法（RIP 反馈融合 + eligible 预算截断）。
 *   Tier1 策略：只能覆盖 quantum 数值与优先级->权重映射等**参数**；
 *                返回 0 表示"沿用内核默认"，因此不改变默认路径行为。
 *
 * ops 回调调用上下文：调度器内部（可能持 sched_lock / 中断关闭），实现必须
 *   不睡眠、不阻塞、不分配内存、不重入调度；单次调用应 < 数微秒。 */
#define KMOD_SEAT_SCHED_POLICY          "kernel.seat.sched.policy.v1"
#define KMOD_SEAT_SCHED_POLICY_ABI      1u

typedef struct kmod_seat_sched_policy_ops {
    uint32_t abi_version;      /* 必须 == KMOD_SEAT_SCHED_POLICY_ABI */
    uint32_t ops_size;         /* 必须 == sizeof(kmod_seat_sched_policy_ops_t) */
    uint32_t policy_version;   /* 0 = 内核默认 EEVDF/反馈；demo v1/v2 = 1/2 */
    uint32_t reserved;         /* 必须 0 */
    const char *identity;      /* 稳定身份字符串（诊断/selftest 可读；非 NULL） */

    /* base_quantum 覆盖（KMOD_SCHED_STAGE_BASE）。返回 0 = 沿用内核默认（ms）。 */
    uint64_t (*base_quantum_ms)(const kmod_sched_policy_ctx_t *ctx);
    /* dynamic/eligibility quantum 覆盖（STAGE_DYNAMIC/ELIGIBILITY）。
     * 返回 0 = 沿用内核默认（ms）。 */
    uint64_t (*quantum_ms)(const kmod_sched_policy_ctx_t *ctx);
    /* 优先级 -> 权重覆盖（对应 sched_prio_to_weight[]）。
     * 返回 0 = 沿用内核默认权重。 */
    uint32_t (*weight_for_priority)(uint32_t priority);
} kmod_seat_sched_policy_ops_t;

/* ------------------------------------------------------------------ */
/* 座位：分配器策略 kernel.seat.alloc.policy.v1（B6）                  */
/* ------------------------------------------------------------------ */
/* 机制/策略边界（原则 5）：
 *   Tier0 固定：PMM/VMM 页表、SLAB/SLUB 分配核心、堆对象布局、失败语义
 *                （失败返回 NULL，不 panic）。
 *   Tier1 策略：只提供冷路径参数（large 阈值 / OOM 决策）；返回 0 = 沿用
 *                内核默认，因此默认 kmalloc 语义/失败语义逐位不变。
 *
 * 性能约束：策略查询只允许在 request >= KMOD_ALLOC_LARGE_MIN(4 KiB) 的
 *   large path 或分配失败后的 OOM 路径；小对象热路径禁止查询（零开销）。 */
#define KMOD_SEAT_ALLOC_POLICY          "kernel.seat.alloc.policy.v1"
#define KMOD_SEAT_ALLOC_POLICY_ABI      1u

typedef struct kmod_seat_alloc_policy_ops {
    uint32_t abi_version;      /* 必须 == KMOD_SEAT_ALLOC_POLICY_ABI */
    uint32_t ops_size;         /* 必须 == sizeof(kmod_seat_alloc_policy_ops_t) */
    uint32_t policy_version;   /* 0 = 内核默认 SLAB/SLUB；demo v1/v2 = 1/2 */
    uint32_t reserved;         /* 必须 0 */
    const char *identity;      /* 稳定身份字符串（诊断/selftest 可读；非 NULL） */

    /* large-allocation 阈值（仅供 request >= 4 KiB 查询）：
     *   入参 request = 本次请求字节数；default_threshold = 内核默认阈值
     *   (KMOD_ALLOC_LARGE_DEFAULT_THRESHOLD)。
     *   返回 0 = 沿用内核默认（不改变路径）；非 0 = 新阈值 t（被 clamp 到
     *   >= KMOD_ALLOC_LARGE_MIN）：request >= t 时直接走 large 路径
     *   （跳过 SLUB 小对象分配尝试；SLUB 尺寸类仅 16..1024B，>=4 KiB 必然
     *   miss，因此默认阈值下结果与不跳过完全一致）。 */
    uint64_t (*large_alloc_threshold)(uint64_t request,
                                      uint64_t default_threshold);
    /* OOM/回退决策（仅分配失败路径查询）：
     *   kind = KMOD_ALLOC_OOM_KIND_*；返回 KMOD_ALLOC_OOM_DEFAULT(0)=沿用
     *   默认（返回 NULL）、KMOD_ALLOC_OOM_RETRY(1)=允许同路径重试一次、
     *   其它正整数=拒绝（同样返回 NULL，仅审计语义不同）。 */
    int32_t (*oom_policy)(uint64_t request, uint32_t kind);
} kmod_seat_alloc_policy_ops_t;

#ifdef __cplusplus
}
#endif

#endif /* _KMOD_SEATS_H_ */
