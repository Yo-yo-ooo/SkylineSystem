// SPDX-License-Identifier: GPL-2.0-only
/* sched_bench.h — REEVDF 反馈环真实内核基准 (时间域画像) */
#pragma once
#include <stdint.h>

namespace SchedBench {

struct step_result_t {
    uint64_t steady_slow;
    uint64_t t90_ms;
    uint64_t fast_peak;
    bool     valid, t90_pass, overshoot_pass, sat_pass;
};
struct pollute_result_t {
    uint64_t base_min, base_max, mult_min, mult_max;
    bool     base_pass, mult_pass;
};
struct shortwin_result_t {
    uint64_t short_windows, base_resets, stalled, mf, msl;
    bool     pass;
};
struct osc_result_t {
    uint64_t weight_min, weight_max, toggles;
    bool     entered_osc, hyst_ok;
};

struct sched_bench_report {
    uint32_t bench_cpu;
    uint32_t base_quantum_ms;   /* 实测: 运行期实际 base quantum (动态自整定) */
    step_result_t     step;
    pollute_result_t  pollute;
    shortwin_result_t shortwin;
    osc_result_t      osc;
};

/* 在 bench_cpu 上运行四相位基准 (要求 ≥2 CPU, 调用者应在别的核) */
bool Run(uint32_t bench_cpu);
sched_bench_report *Report(void);

} // namespace SchedBench
