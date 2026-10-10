// SPDX-License-Identifier: GPL-2.0-only
// TSC Calibrate (SYNC TO PIT)
#include <stdint.h>
#include <klib/kprintf.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/drivers/hpet/hpet.h>

static uint64_t g_tsc_per_ms = 0;

static inline uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 早期启动调用 (调度器初始化前): 量 TSC 的每毫秒周期数。
   基准用 HPET 主计数器 —— 它是 MMIO 直读, 不依赖任何中断投递。
   原实现以 PIT::TimeSinceBootMS() 为基准, 而该值只在 BSP 的 IRQ0 中断
   上下文里递增; 本函数在启动路径上执行时 BSP 常常处于关中断窗口, 实测
   PIT 窗口恒为 0ms -> g_tsc_per_ms 永远是 0 -> 所有以 MonotonicMS() 为
   deadline 的等待 (TLB shootdown ACK、进程退出等待) 全部退化成自旋上限
   兜底, 且 PIT::TimeSinceBootMS() 的冻结保护也一并失效。 */
void sched_calibrate_tsc(void) {
    if (!HPET::Available()) {
        kerrorln("tsc: calibration FAILED (no HPET timebase) - "
                 "deadline fallbacks degraded to spin limits");
        return;
    }

    /* 先确认计数器确实在推进 (有界探测), 否则下面的忙等会变成长时间自旋 */
    uint64_t t0 = HPET::GetTimeNS();
    uint64_t probe = 0;
    while (HPET::GetTimeNS() == t0 && probe < 100000ULL) probe++;
    if (HPET::GetTimeNS() == t0) {
        kerrorln("tsc: calibration FAILED (HPET counter not advancing) - "
                 "deadline fallbacks degraded to spin limits");
        return;
    }

    t0 = HPET::GetTimeNS();
    uint64_t c0 = tsc_now();
    uint64_t guard = 0;
    while (HPET::GetTimeNS() - t0 < 100000000ULL && guard < 2000000000ULL) guard++;
    uint64_t c1 = tsc_now();
    uint64_t ns = HPET::GetTimeNS() - t0;

    if (ns >= 50000000ULL)                      /* ≥50ms 有效窗口才采信 */
        g_tsc_per_ms = (c1 - c0) * 1000000ULL / ns;

    /* 合理性校验: HPET 的 counter_clk_period 若解析异常会算出荒谬频率, 而
       g_tsc_per_ms 一旦被污染, 所有 MonotonicMS() deadline 会集体失准 (要么
       立刻超时要么永不超时)。拒收 100MHz~100GHz 之外的值。 */
    if (g_tsc_per_ms &&
        (g_tsc_per_ms < 100000ULL || g_tsc_per_ms > 100000000ULL)) {
        kerrorln("tsc: calibration result %llu cycles/ms out of range - rejected",
                 g_tsc_per_ms);
        g_tsc_per_ms = 0;
    }

    /* 校准结果必须可见: PIT::TimeSinceBootMS() 的冻结保护、以及所有以
       PIT::MonotonicMS() 为 deadline 的等待, 都依赖 g_tsc_per_ms != 0。
       未校准时它们全部退化成"靠自旋次数兜底", 必须让日志说清楚。 */
    if (g_tsc_per_ms) {
        kinfoln("tsc: calibrated %llu cycles/ms (HPET window %lluns)",
                g_tsc_per_ms, ns);
    } else {
        kerrorln("tsc: calibration FAILED (HPET window only %lluns < 50ms) - "
                 "deadline fallbacks degraded to spin limits", ns);
    }
}

/* 强符号: 覆盖 sched_rip.cpp 的弱符号 */
uint64_t sched_tsc_per_ms(void) { return g_tsc_per_ms; }
