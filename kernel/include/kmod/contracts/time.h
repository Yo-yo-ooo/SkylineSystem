//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * kernel/include/kmod/contracts/time.h —— 时间相关私有契约（v1）
 * ============================================================================
 * 提供者组件（B1 时间链）：
 *   kernel.time.pit   -> kernel.time.v1   单调毫秒 + IRQ0 节拍计数
 *   kernel.time.hpet  -> kernel.hpet.v1   HPET 纳秒计数（不可用时 available=0）
 *   kernel.time.rtc   -> kernel.rtc.v1    CMOS RTC 读秒/转 Unix 时间
 * 消费者: 诊断/审计组件、未来时间策略 .ko、需要只读时间的组件。
 *
 * 注意：Tier0 内核导出表（.kout）里也有一个 "kernel.time.v1" 契约，它只导出
 * KmodNowMs（gkm 平台毫秒）。两者**按 provider 区分**：providers="kernel" 时
 * 由内核导出表回填，provider="kernel.time.pit" 时由本契约组件回填；内核按
 * provider+symbol 机械匹配，不解释同名语义。
 * ==========================================================================*/
#ifndef _KMOD_CONTRACT_TIME_H_
#define _KMOD_CONTRACT_TIME_H_

#include <stdint.h>
#include <stddef.h>
#include <kmod/kmod_abi.h>   /* KMOD_ABI_STATIC_ASSERT */

/* ---------------- kernel.time.v1（PIT 单调时钟） ---------------- */
#define KMOD_CONTRACT_TIME_V1   "kernel.time.v1"
#define KMOD_TIME_ABI_V1        1u

typedef struct kmod_time_ops {
    uint32_t abi_version;          /* = KMOD_TIME_ABI_V1 */
    uint32_t ops_size;             /* = sizeof(kmod_time_ops_t) */
    /* 单调毫秒：已校准时走 TSC（不受 IF/中断路由影响），否则退回 PIT 累计。 */
    uint64_t (*monotonic_ms)(void);
    /* 自启动以来的 IRQ0 节拍计数（不一定 = 毫秒）。 */
    uint64_t (*ticks)(void);
} kmod_time_ops_t;

KMOD_ABI_STATIC_ASSERT(sizeof(kmod_time_ops_t) == 24u,
                       "kmod_time_ops_t ABI freeze");

/* ---------------- kernel.hpet.v1（高精度事件定时器） ---------------- */
#define KMOD_CONTRACT_HPET_V1   "kernel.hpet.v1"
#define KMOD_HPET_ABI_V1        1u

typedef struct kmod_hpet_ops {
    uint32_t abi_version;          /* = KMOD_HPET_ABI_V1 */
    uint32_t ops_size;             /* = sizeof(kmod_hpet_ops_t) */
    /* 主计数器纳秒值；available()==0 时无意义（旧实现会退化为 tick 计数）。 */
    uint64_t (*get_time_ns)(void);
    /* HPET 主计数器是否真实可用（调用方必须先查）。 */
    int32_t (*available)(void);
} kmod_hpet_ops_t;

KMOD_ABI_STATIC_ASSERT(sizeof(kmod_hpet_ops_t) == 24u,
                       "kmod_hpet_ops_t ABI freeze");

/* ---------------- kernel.rtc.v1（CMOS 实时钟） ---------------- */
#define KMOD_CONTRACT_RTC_V1    "kernel.rtc.v1"
#define KMOD_RTC_ABI_V1         1u

typedef struct kmod_rtc_ops {
    uint32_t abi_version;          /* = KMOD_RTC_ABI_V1 */
    uint32_t ops_size;             /* = sizeof(kmod_rtc_ops_t) */
    /* 当前 RTC 时间的 Unix 秒（只读；内部按需刷新 CMOS）。 */
    uint32_t (*to_unix_time)(void);
    /* 强制从 CMOS 重新读取并更新缓存的时间字段。 */
    void (*read)(void);
} kmod_rtc_ops_t;

KMOD_ABI_STATIC_ASSERT(sizeof(kmod_rtc_ops_t) == 24u,
                       "kmod_rtc_ops_t ABI freeze");

#endif /* _KMOD_CONTRACT_TIME_H_ */
