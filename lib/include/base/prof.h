//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: MIT
//
// base/prof.h — 用户态轻量性能探针 (TSC 时基 + 延迟分位数)
// ------------------------------------------------------------------------
// 只为"输入延迟可量化"而存在 (docs/desktop-features.md 第 9 节):
//   * sky_rdtsc()        读 TSC (用户态可执行, 与桌面主循环用的是同一条指令);
//   * sky_tsc_per_ms()   每个毫秒的 TSC 周期数。优先取内核用 HPET 校准过的
//                        值 (SysInfo.reserved[0], 与 desktop 的 probe 一致),
//                        取不到时用 sys_monotonic_ms 现标一次 (50 ms 窗口);
//   * SkyLat             32 个样本的环形缓冲, 满了算一次 p50/p99 再清零。
//                        32 个样本用插入排序, 成本可以忽略; p99 取样本最大值
//                        (32 个样本的 99 分位就是最大值, 报告里不要再当成
//                        真正的 1/100 分位去解读)。
//
// 所有函数都是 static inline: 不新增 libsky 导出符号, 不改 ABI。
#pragma once

#include <stdint.h>
#include <base/arch/x86_64/syscall.h>
#include <base/arch/x86_64/syscalln.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t sky_rdtsc(void) {
    uint32_t lo = 0, hi = 0;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t sky_now_ms(void) {
    return (uint64_t)syscall(SYSCALL_MONOTONIC_MS, 0, 0, 0, 0, 0, 0);
}

/* TSC 周期/毫秒。结果缓存在函数级静态变量里 (每个 TU 一份, 代价可忽略)。 */
static inline uint64_t sky_tsc_per_ms(void) {
    static uint64_t cached = 0;
    if (cached) return cached;

    /* 1) 内核 HPET 校准值 —— 最准, 且与内核/桌面完全一致。 */
    SysInfo *si = (SysInfo*)sys_sysinfo(0);
    if ((int64_t)si > 0) {
        const uint64_t v = si->reserved[0];
        sys_sysinfo((uint64_t)si);            /* 归还映射 */
        if (v >= 100000ull && v <= 100000000ull) {
            cached = v;
            return cached;
        }
    }

    /* 2) 现场标定: 用毫秒级单调时钟量 50 ms 的 TSC 增量。
          粒度 1 ms, 50 ms 窗口的相对误差 ~2%, 足够判断 p50/p99 的量级。 */
    const uint64_t t0 = sky_rdtsc();
    const uint64_t m0 = sky_now_ms();
    uint64_t m1 = m0;
    while (m1 - m0 < 50) m1 = sky_now_ms();
    const uint64_t t1 = sky_rdtsc();
    const uint64_t dms = m1 - m0;
    cached = dms ? (t1 - t0) / dms : 1000000ull;
    if (cached == 0) cached = 1000000ull;
    return cached;
}

/* TSC 增量 -> 微秒。 */
static inline uint64_t sky_tsc_to_us(uint64_t dt) {
    const uint64_t per_ms = sky_tsc_per_ms();
    if (!per_ms) return 0;
    return (dt * 1000ull) / per_ms;
}

#define SKY_LAT_SAMPLES 32

typedef struct SkyLat {
    uint32_t us[SKY_LAT_SAMPLES];
    int      n;
} SkyLat;

static inline void sky_lat_reset(SkyLat *l) {
    if (!l) return;
    l->n = 0;
}

/* 记录一个样本。返回 1 表示这一批已经攒满, 调用方应调用 sky_lat_report()。 */
static inline int sky_lat_add(SkyLat *l, uint64_t us) {
    if (!l) return 0;
    if (us > 0xFFFFFFFFull) us = 0xFFFFFFFFull;
    l->us[l->n++] = (uint32_t)us;
    if (l->n < SKY_LAT_SAMPLES) return 0;
    return 1;
}

/* 算分位数并清零。p50/p99 都是下标取整 (p99 = 样本最大值)。 */
static inline void sky_lat_report(SkyLat *l, uint32_t *p50, uint32_t *p99, int *n) {
    if (!l) return;
    uint32_t s[SKY_LAT_SAMPLES];
    const int cnt = l->n;
    for (int i = 0; i < cnt; i++) s[i] = l->us[i];
    for (int i = 1; i < cnt; i++) {            /* 插入排序 (cnt <= 32) */
        const uint32_t v = s[i];
        int j = i - 1;
        while (j >= 0 && s[j] > v) { s[j + 1] = s[j]; j--; }
        s[j + 1] = v;
    }
    if (p50) *p50 = cnt ? s[cnt / 2] : 0u;
    if (p99) *p99 = cnt ? s[cnt - 1] : 0u;
    if (n)   *n   = cnt;
    l->n = 0;
}

#ifdef __cplusplus
}
#endif
