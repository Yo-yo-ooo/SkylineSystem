// SPDX-License-Identifier: GPL-2.0-only
/* tsc_cal.cpp — TSC 每毫秒周期数校准 (sched_tsc_per_ms 强符号)
 *
 * sched_rip.cpp 默认弱符号返回 0 = 未校准 → RIP 采样走墙钟分母,
 * 中断/抢占时间混入 → 竞争场景下倍率振荡 (sched_bench pollute 相位
 * 真内核实测 base=[41,2057] 的候选根因之一)。
 * 此处以 PIT 毫秒为基准实测 TSC 频率, 校准后采样走执行期分母。 */
#include <stdint.h>
#include <arch/x86_64/pit/pit.h>

static uint64_t g_tsc_per_ms = 0;

static inline uint64_t tsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 早期启动调用 (调度器初始化前): 以 PIT 为基准量 TSC。
   忙等 100ms, 带有界守卫 (早期 PIT 可能尚未走时 —— 实测会永旋,
   卡死启动) + 进度校验 (PIT 未推进则放弃, 保持未校准语义) */
void sched_calibrate_tsc(void) {
    uint64_t t0 = PIT::TimeSinceBootMS();
    uint64_t c0 = tsc_now();
    /* 守卫 = 毫秒级: PIT 若在数毫秒自旋内不推进即冻结 (早期启动),
       立即放弃 —— 大守卫 (4e9) 在冻结态要自旋数分钟, 曾卡死启动 */
    uint64_t guard = 0;
    while (PIT::TimeSinceBootMS() - t0 < 100 && guard < 2000000ULL) { guard++; }
    uint64_t c1 = tsc_now();
    uint64_t ms = PIT::TimeSinceBootMS() - t0;
    if (ms >= 50) g_tsc_per_ms = (c1 - c0) / ms;   /* 有效窗口才采信 */
}

/* 强符号: 覆盖 sched_rip.cpp 的弱符号 */
uint64_t sched_tsc_per_ms(void) { return g_tsc_per_ms; }
