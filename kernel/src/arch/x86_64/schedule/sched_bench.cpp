// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// sched_bench.cpp — REEVDF 反馈环基准
//
// 只依赖公开接口: NewProcess / NewKernelThread / Exit / Yield /
// this_thread / GetRipStats / PIT::TimeSinceBootMS / thread_t 字段.
// 集成点 (两处):
//   1. 打印: 结果在 Report() 返回的结构里, 用现有 kprintf/proc
//      接口格式化输出;
//   2. 基准 4 用 "相位交替" 模拟周期突发 (无公开 sleep API),
//      有 sleep 的话把相位间直接切换换成 sleep 更纯净.
// 运行前提: >= 2 CPU — Run(bench_cpu) 的调用者应在别的 CPU 上
// (调用者会作为第三线程污染 bench_cpu 的测量). UP 上仅供参考.
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/pit/pit.h>
#include <arch/x86_64/smp/smp.h>
#include <pdef.h>

namespace SchedBench {

/* ---- 可调参数 ---- */
enum { STEP_A_MS = 400, STEP_B_MS = 600, STEP_RING = 128,
       POLLUTE_MS = 10000, POLLUTE_RING = 64,
       SHORTWIN_MS = 5000,
       OSC_CYCLES = 100, OSC_HALF_MS = 50 };

struct step_sample { uint64_t t_ms, mf, msl; };
struct step_result_t {
    uint64_t steady_slow;      /* Q10, 相位 B 末 16 样本均值 */
    uint64_t t90_ms;           /* 切换到 90% 到位的时间 */
    uint64_t fast_peak;        /* Q10 */
    bool     valid, t90_pass, overshoot_pass, sat_pass;
};
struct pollute_sample { uint64_t base, mult; };
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
    step_result_t     step;
    pollute_result_t  pollute;
    shortwin_result_t shortwin;
    osc_result_t      osc;
};
static sched_bench_report g_report;

/* ---- 共享状态 (线程入口无参数 → 静态) ---- */
static volatile uint64_t g_sink;
static volatile uint64_t g_chase[64];
static volatile uint32_t g_flag_run;         /* companion/polluter 停止开关 */
static volatile uint32_t g_done;
static step_sample    g_sring[STEP_RING];     static volatile uint64_t g_sring_n;
static pollute_sample g_pring[POLLUTE_RING];  static volatile uint64_t g_pring_n;
static volatile uint64_t g_sw_mf, g_sw_msl;   /* 短窗口目标线程终值 */

/* ---- 两种速率的循环体 (volatile 防优化) ---- */
static inline void fast_body(unsigned n) {          /* 纯 ALU: 高 RIP 速率 */
    uint64_t x = g_sink;
    for (unsigned i = 0; i < n; i++) x = x * 1664525u + 1013904223u;
    g_sink = x;
}
static inline void slow_body(unsigned n) {          /* 指针追猎: 低 RIP 速率 */
    uint64_t idx = 0;
    for (unsigned i = 0; i < n; i++) idx = g_chase[idx & 63];
    g_sink = idx;
}

static void run_phase(bool fast, uint64_t dur_ms, bool record) {
    uint64_t t0 = PIT::TimeSinceBootMS(), next = t0 + 4;
    while (PIT::TimeSinceBootMS() - t0 < dur_ms) {
        if (fast) fast_body(2000); else slow_body(64);
        if (record) {
            uint64_t now = PIT::TimeSinceBootMS();
            if (now >= next && g_sring_n < STEP_RING) {
                thread_t *t = Schedule::this_thread();
                if (likely(t)) {
                    g_sring[g_sring_n].t_ms = now - t0;
                    g_sring[g_sring_n].mf   = t->rip_mult_fast;
                    g_sring[g_sring_n].msl  = t->rip_mult_slow;
                    g_sring_n++;
                }
                next = now + 4;
            }
        }
    }
}

/* ---- 基准 1: 阶跃响应 ---- */
static void bench_step_thread() {
    run_phase(false, STEP_A_MS, false);   /* 建基线 (含首采样, 无反馈) */
    g_sring_n = 0;
    run_phase(true, STEP_B_MS, true);
    g_done = 1;
    Schedule::Exit(0);
}
static void eval_step(step_result_t *r, const uint64_t pre[14], const uint64_t post[14]) {
    uint64_t n = g_sring_n;
    r->valid = false; r->t90_pass = r->overshoot_pass = r->sat_pass = false;
    if (n < 32) return;
    uint64_t sum = 0;
    for (uint64_t i = n - 16; i < n; i++) sum += g_sring[i].msl;
    uint64_t steady = sum / 16;
    r->steady_slow = steady;
    if (steady < 1024 + 512) return;      /* 阶跃幅度 < 1.5x, 测试无效 */
    uint64_t target = 1024 + (steady - 1024) * 9 / 10;   /* 1.0 + 0.9×阶跃量 */
    uint64_t t90 = 0; bool hit = false;
    for (uint64_t i = 0; i < n; i++)
        if (g_sring[i].msl >= target) { t90 = g_sring[i].t_ms; hit = true; break; }
    r->t90_ms = hit ? t90 : (uint64_t)-1;
    uint64_t peak = 0;
    for (uint64_t i = 0; i < n; i++) if (g_sring[i].mf > peak) peak = g_sring[i].mf;
    r->fast_peak = peak;
    r->valid = true;
    /* 1/16 EWMA 阶跃收敛 (15/16)^n: 90% 需要 ~36 样本 ≈ 180ms
     * @5ms 量子 → 判据 250ms 留余量. 按 32 样本设判据会对正确
     * 调参的环产生假 FAIL (32 样本只有 87.5%). */
    r->t90_pass = hit && t90 <= 250;
    r->overshoot_pass = peak * 5 < steady * 6;          /* < 20% 超调 */
    r->sat_pass = (post[6] - pre[6]) + (post[7] - pre[7]) < 5;
}

/* ---- 基准 2: 抗污染 ----
 * victim 自身速率恒定; polluter 高速率. per-thread 基线下
 * victim 的 mult 应稳定在 1.0x — 共享基线会把 victim 的 mult
 * 砸到下限, 这是区分两种基线设计的直接证据. */
static void bench_polluter() { while (g_flag_run) fast_body(2000); Schedule::Exit(0); }
static void bench_victim() {
    uint64_t t0 = PIT::TimeSinceBootMS(), next = t0 + 100;
    while (PIT::TimeSinceBootMS() - t0 < POLLUTE_MS) {
        slow_body(64);                    /* victim 自身速率恒定 */
        uint64_t now = PIT::TimeSinceBootMS();
        if (now >= next && g_pring_n < POLLUTE_RING) {
            thread_t *t = Schedule::this_thread();
            if (likely(t)) {
                g_pring[g_pring_n].base = t->rip_base_self;
                g_pring[g_pring_n].mult = t->rip_mult_slow;
                g_pring_n++;
            }
            next = now + 100;
        }
    }
    g_done = 1;
    Schedule::Exit(0);
}
static void eval_pollute(pollute_result_t *r) {
    uint64_t n = g_pring_n;
    r->base_min = r->base_max = r->mult_min = r->mult_max = 0;
    r->base_pass = r->mult_pass = false;
    if (n < 16) return;
    uint64_t bmin = ~0ULL, bmax = 0, mmin = ~0ULL, mmax = 0;
    for (uint64_t i = 8; i < n; i++) {    /* 跳过前 8 个: 基线建立期 */
        if (g_pring[i].base < bmin) bmin = g_pring[i].base;
        if (g_pring[i].base > bmax) bmax = g_pring[i].base;
        if (g_pring[i].mult < mmin) mmin = g_pring[i].mult;
        if (g_pring[i].mult > mmax) mmax = g_pring[i].mult;
    }
    r->base_min = bmin; r->base_max = bmax;
    r->mult_min = mmin; r->mult_max = mmax;
    r->base_pass = bmax * 4 < bmin * 5;   /* 极差 < 25% */
    r->mult_pass = (mmin >= 820 && mmax <= 1230);
}

/* ---- 基准 3: 短窗口防御 ----
 * prio 15 (weight 288) × base 5ms → 量子 ~1.4ms < 3ms 门.
 * companion (prio 10) 保持 CPU 忙, 防止空闲自整定把 base 量子
 * 拉长导致门失效. */
static void bench_sw_companion() { while (g_flag_run) fast_body(2000); Schedule::Exit(0); }
static void bench_sw_target() {
    uint64_t t0 = PIT::TimeSinceBootMS();
    while (PIT::TimeSinceBootMS() - t0 < SHORTWIN_MS) fast_body(2000);
    /* 终值在 Exit 前自采样: 线程变僵尸后可能已被回收,
     * 外部持有的指针不可再解引用 */
    thread_t *t = Schedule::this_thread();
    if (likely(t)) { g_sw_mf = t->rip_mult_fast; g_sw_msl = t->rip_mult_slow; }
    g_done = 1;
    Schedule::Exit(0);
}
static void eval_shortwin(shortwin_result_t *r, const uint64_t pre[14], const uint64_t post[14]) {
    r->short_windows = post[3] - pre[3];
    r->base_resets   = post[8] - pre[8];
    r->stalled       = post[4] - pre[4];
    r->mf = g_sw_mf; r->msl = g_sw_msl;
    r->pass = r->short_windows >= 200 && r->base_resets == 0
           && g_sw_mf >= 820 && g_sw_mf <= 1230
           && g_sw_msl >= 820 && g_sw_msl <= 1230;
}

/* ---- 基准 4: 振荡检测 (相位交替近似周期突发) ---- */
static void bench_osc_thread() {
    for (int c = 0; c < OSC_CYCLES; c++) {
        run_phase(false, OSC_HALF_MS, false);
        run_phase(true,  OSC_HALF_MS, false);
    }
    g_done = 1;
    Schedule::Exit(0);
}

/* ---- 编排 ---- */
static void wait_done(uint64_t timeout_ms) {
    uint64_t t0 = PIT::TimeSinceBootMS();
    while (!g_done && PIT::TimeSinceBootMS() - t0 < timeout_ms) Schedule::Yield();
}
static void settle(uint64_t ms) {   /* 等僵尸回收/伴线程退出 */
    uint64_t t0 = PIT::TimeSinceBootMS();
    while (PIT::TimeSinceBootMS() - t0 < ms) Schedule::Yield();
}

sched_bench_report *Report() { return &g_report; }

bool Run(uint32_t bench_cpu) {
    if (unlikely(bench_cpu > (uint32_t)smp_last_cpu)) return false;
    for (int i = 0; i < 64; i++) g_chase[i] = (uint64_t)((i * 17 + 13) & 63);
    g_report = {};
    g_report.bench_cpu = bench_cpu;
    proc_t *proc = Schedule::NewProcess(false);
    if (unlikely(!proc)) return false;

    uint64_t pre[14], post[14];

    /* 1. 阶跃 */
    Schedule::GetRipStats(bench_cpu, pre);
    g_done = 0; g_sring_n = 0;
    Schedule::NewKernelThread(proc, bench_cpu, 8, (void*)bench_step_thread);
    wait_done(STEP_A_MS + STEP_B_MS + 3000);
    Schedule::GetRipStats(bench_cpu, post);
    eval_step(&g_report.step, pre, post);
    settle(200);

    /* 2. 抗污染 */
    Schedule::GetRipStats(bench_cpu, pre);
    g_done = 0; g_pring_n = 0; g_flag_run = 1;
    Schedule::NewKernelThread(proc, bench_cpu, 8, (void*)bench_polluter);
    Schedule::NewKernelThread(proc, bench_cpu, 8, (void*)bench_victim);
    wait_done(POLLUTE_MS + 5000);
    g_flag_run = 0;
    Schedule::GetRipStats(bench_cpu, post);
    eval_pollute(&g_report.pollute);
    settle(200);

    /* 3. 短窗口 */
    Schedule::GetRipStats(bench_cpu, pre);
    g_done = 0; g_flag_run = 1; g_sw_mf = g_sw_msl = 0;
    Schedule::NewKernelThread(proc, bench_cpu, 10, (void*)bench_sw_companion);
    Schedule::NewKernelThread(proc, bench_cpu, 15, (void*)bench_sw_target);
    wait_done(SHORTWIN_MS + 5000);
    g_flag_run = 0;
    Schedule::GetRipStats(bench_cpu, post);
    eval_shortwin(&g_report.shortwin, pre, post);
    settle(200);

    /* 4. 振荡: 运行期轮询融合权重 */
    g_done = 0;
    Schedule::NewKernelThread(proc, bench_cpu, 8, (void*)bench_osc_thread);
    uint64_t wmin = ~0ULL, wmax = 0, toggles = 0, lastw = 0;
    uint64_t t0 = PIT::TimeSinceBootMS();
    uint64_t dur = (uint64_t)OSC_CYCLES * 2 * OSC_HALF_MS;
    while (!g_done && PIT::TimeSinceBootMS() - t0 < dur + 5000) {
        uint64_t o[14];
        if (Schedule::GetRipStats(bench_cpu, o)) {
            uint64_t w = o[11];
            if (w < wmin) wmin = w;
            if (w > wmax) wmax = w;
            if (lastw != 0 && w != lastw) toggles++;
            lastw = w;
        }
        uint64_t poll_t0 = PIT::TimeSinceBootMS();
        while (PIT::TimeSinceBootMS() - poll_t0 < 50) fast_body(64);
    }
    g_report.osc.weight_min = (wmin == ~0ULL) ? 0 : wmin;
    g_report.osc.weight_max = wmax;
    g_report.osc.toggles = toggles;
    g_report.osc.entered_osc = (wmax <= 64);        /* 到过慢模式 */
    g_report.osc.hyst_ok = (toggles <= 4);          /* 迟滞有效, 无乒乓 */
    return true;
}

} // namespace SchedBench