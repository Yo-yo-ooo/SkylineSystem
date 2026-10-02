# Scheduler — deadline-keyed scheduling + RIP rate feedback (formerly named 3EVDF)

Source: `kernel/src/arch/x86_64/schedule/sched.cpp`, `task.cpp`, `timer.cpp`, `syscall/`.

> This document only describes what the code **actually does**, and explicitly lists what it
> **does not do**.

## 1. Actual Structure

Each CPU has a **red-black tree** run queue keyed by **virtual deadline** (P3-74 correction:
originally written "keyed by vruntime" — actually on enqueue `deadline = vruntime +
base_quantum`, the queue is sorted by deadline; since deadline ≡ vruntime + constant, the order
is equivalent to vruntime order, but the key field itself is deadline), nodes augmented with
`min_vruntime_subtree` (subtree minimum), comparison points carry `PREFETCH_R`. Threads get a
base time slice by weight (`sched_prio_to_weight[16]`), `vruntime` advances by **real elapsed
milliseconds × 1024 / weight**, `avg_vruntime` advances at the load-weighted rate.

`Pick()` selects using only `vruntime` and `avg_vruntime`: descends along the branch whose
"subtree contains an eligible node", taking the minimum.

## 2. RIP-progress-rate Feedback (the 3EVDF part) — what it changes, what it does not

Each tick samples the current thread's RIP progress, runs it through Q10 fixed-point dual-
channel EWMA to get a fast/slow multiplier `mult` (clamped to [0.25x, 4x]), plus a signed
correction term `adj` of ±4 ticks, combining into **each thread's time-slice length**:

```text
eff_quantum = (base × fused_mult >> 10) + adj
```

This result is used only for **the LAPIC oneshot timer duration** (the call site of
`get_dynamic_quantum()` in `sched.cpp`).

**What it does not change (important)**:

- `Pick()` does **not** see `mult` / `adj` — selection is still decided by `vruntime`;
- `vruntime` is accounted by real time, **not scaled by the multiplier**;
- therefore the feedback affects the **preemption rhythm/interrupt latency** (busy-wait threads
  get interrupted more often, fast-progressing threads get longer uninterrupted runs), and
  **does not change CPU shares**;
- fairness is carried by the vruntime mechanism, orthogonal to the rate feedback.

## 3. How Real the EEVDF Components Are

- `calibrate_and_set_deadline()` clamps `vruntime` into `[avg−q, avg+2q]` on **every enqueue**
  and sets `deadline = vruntime + base_quantum`;
- the time slice is **already weight-scaled** (P3-75 correction: the original document wrote
  "not weight-scaled" — actually `base_quantum` is scaled by `sched_prio_to_weight[priority]`
  and used as the deadline's constant term), deadline ≡ scaled vruntime + constant;
- so this is a **subset of the EEVDF structure**, not the complete EEVDF. Both README and this
  document describe it as such.

## 4. Dynamic Base Quantum and Aging

`dynamic_adjust_quantum()` adjusts `base_quantum` (clamped to [2,15] ticks) every 100ms based
on the idle ratio / context-switch count. On the rate-feedback side:

- no sample for 50ms → `mult` shrinks toward 1.0 in 1/16 steps, `adj` shrinks toward 0 (source
  comments record the old bug where `(x+7)>>4` is always 0 for |adj|≤8);
- outlier observations (>16x) clamped to 16x;
- new threads with `mult==0` treated as 1.0.

## 5. SMP Load Balancing

- **Active push (`TryPush`)**: when this core has surplus and ≥2 threads, find the lightest
  target by total weight, preferring the **same SIMD feature mask** (note: `cpu_simd_mask()` is
  an SIMD instruction-set bitmap, **not SMT topology**; the kernel has no APIC-ID/core-sibling
  data), double lock ordered by `cpu.id` to prevent ABBA, batch `SCHED_STEAL_BATCH=8`, after
  pushing if the target is hlt send an IPI wakeup immediately.
- **Lazy steal (`StealThread`)**: sweep once every 8 throttles; same mask first, then any core;
  `spin_trylock` cap 100 attempts; after stealing, first mark `THREAD_TRANSFER` then swap locks
  and enqueue.

## 6. Preemption and Context Switch

`Schedule::Switch()` (SCHED_VEC ISR): stop the LAPIC timer first → `this_cpu==nullptr` still
EOI → AP early-window re-arm and return → `preempt_count > 1` does not clear flags, only
rearranges oneshot → voluntary `yield()` goes through a separate `yield_request_flags`.

**Known defect (unfixed)**: `CheckPreempt()` has no callers anywhere in the code tree; the
re-trigger after `preempt_count` reaches zero depends on the next timer tick.

## 7. Known Fixes and New-Thread Accounting

New threads' `last_run_time` was previously uninitialized at all creation sites (first slice
`delta = uptime`, launching vruntime upward); it is now **initialized at creation sites** with a
defensive check for `==0` at the accounting point. All thread creation sites assign after
`memset`, behavior is deterministic.

## 8. Test Status (Honest)

**The real-kernel scheduling quality benchmark has run through (round 24).** `sched_bench`'s
four phases run completely on the real kernel (QEMU, cpu 1 dedicated thread, network stack
delayed = quiet machine):

| Phase | Result | Interpretation |
|---|---|---|
| Step (RIP feedback convergence) | **t90=4ms, no overshoot ✓** | REEVDF feedback loop converges very fast in a quiet environment (193ms in a polluted environment) |
| Pollution resistance | failed: base=[41,2057] mult=[785,2211] | real kernel differs from the model → **to investigate** |
| Short window | failed: windows=0 stalled=5 | short windows not detected → **to investigate** |
| Oscillation hysteresis | w=[64,64] toggles=0 ✓ | hysteresis effective, no ping-pong |

**The two failed phases = the real gap between the real-kernel implementation and the model
tests** (model tests pass, real kernel fails → not a benchmark bug, but the RIP feedback
implementation's behavioral difference in pollution/short-window scenarios), i.e. the entry
point for the next round of scheduler debugging.

Round 25 root-cause-analysis increments:
- **shortwin failed (windows=0)**: `dynamic_adjust_quantum` dynamically clamps base_quantum
  to [2,15]ms and self-tunes by load; the bench's static assumption (base=5ms → prio15 weight
  288 → slice ~1.4ms < 3ms gate) breaks under the dynamic quantum (at base=15ms the slice is
  ~4.2ms > gate → 0 short windows). **Not a scheduling defect; a mismatch between the benchmark
  semantics and the dynamic-quantum characteristic** → the bench should read the actual
  base_quantum or freeze self-tuning during the phase.
- **pollute failed (multiplier swing 41..2057)**: the victim's RIP multiplier oscillates
  heavily under polluter contention — stable in model tests, oscillating in the real kernel =
  real-kernel-specific behavior (candidates: interrupt/preemption noise entering samples,
  `sched_tsc_per_ms` uncalibrated distorting the wall-clock denominator). **Needs retest after
  TSC calibration**, or investigate the feedback gain's stability under contention.

Round 26 increments (TSC calibration + retest):
- Implemented `sched_calibrate_tsc()` (tsc_cal.cpp, PIT baseline, busy-wait with millisecond-
  scale guard — the early version hung at boot when the guard was too large while the PIT
  wasn't running yet) + `sched_tsc_per_ms` strong symbol.
- Retest: step t90=42ms sat=1 (saturation detection appears); **pollute still fails**
  (base=[80,1322] mult=[979,2560]) → after eliminating the denominator-distortion candidate,
  points to **a real stability problem of the RIP feedback gain under contention** (the
  feedback loop itself oscillates, gain analysis pending).
- shortwin still windows=0 (dynamic quantum mismatch, same conclusion as round 25).
Round 28 root-cause closure (the complete mechanism of pollute oscillation, D1 round 23
refiled):
- Sampling structure: sample per slice tick, `obs_rate = RIP delta / own slice length
  (last_slice_ms)` — the denominator is correct (the thread's own run duration, not the
  wall-clock gap).
- **Oscillation source = preemption-driven variable-length slices**: under EEVDF, when
  equally-favored polluters compete, the victim's slice gets truncated by timer/deadline
  preemption → short slices' RIP deltas are systematically low → obs_rate alternates high/low →
  the fast channel (1/4 EWMA) follows the swing (the mechanism behind base [41,2057]).
- Sub-millisecond slices forced to delta=1 → further amplifies short-slice underestimation.
- **Fix design (to implement)**: change sampling to **accumulator-style fixed windows** — 
  accumulate RIP delta and duration by the thread's own run time (e.g. one obs_rate sample per
  4ms of own run time), variable-slice noise absorbed by the accumulator; the short-window
  defense (RIP_MIN_SAMPLE_MS) simplified along with it.
Round 33 (accumulator landed + UAF confirmed):
- Accumulator sampling implemented (thread_t gains rip_acc_progress/ms, 4ms window per
  sample); first test pollute base=[41,2057]→[51,724] (3x narrowing), after correcting the
  denominator to own slice length (wall_ms; exec_ms's tsc_dt spans gaps including other
  threads' slices, semantically wrong) awaiting retest.
- **bench Exit-path page fault symbolically located**: RIP=atomic_test_and_set (spinlock
  primitive), CR2=0xFFFF90000211D044 (a lock word inside a kmalloc object), on thread 16 →
  **spinlock UAF on the exit/reclaim path**: some struct (containing a lock) was freed while
  another thread still spins on its lock. A real scheduler defect; reclaim audit = follow-up
  task.

Round 34 (retest + UAF suspicion):
- wall_ms denominator-correction retest: pollute base=[51,724] same as before correction →
  remaining variance is on the numerator/phase-transition side (the 4ms window straddles the
  polluter switch point), needs per-sample instrumentation within the window — convergence
  improvement achieved (3x), full pass left for later.
- UAF suspicion narrowed: proc zombie reclamation (DrainProcZombieList) frees FDMan/pagemap
  without global synchronization, and FDMan contains a lock table — the most likely source of
  the lock-word UAF; the fix needs a synchronization audit of fd_manager_destroy and the
  reclamation window.

Round 40 (UAF fix) + round 41 (defense in depth):
- Root cause: phase threads' `Schedule::Exit(0)` kills the entire bench-internal proc;
  subsequent phase threads spawn into the same concurrently-terminated proc → FDMan lock-word
  UAF (symbolized in round 33).
- Fix: phase threads no longer Exit (changed to hlt idle-spin; wait_done already advances by
  timeout) + NewKernelThreadEx/NewThread gain an exiting guard (spawning into an exiting proc
  fails explicitly) → the UAF debt fully closed.

Round 42 (full report after the UAF fix):
- pollute **base phase passes** (base=[1067,1309], max deviation <25% — the first passing item
  from the accumulator fix); mult phase [962,1463] still exceeds the [820,1230] bound (mult
  target = the victim's multiplier when the polluter runs at multiplier, residual variance
  awaiting in-window instrumentation).
- osc this run entered=0 (w stopped at 256, phase variant); step t90=176ms ✓.
- **0 anomalies**: after the UAF fix the whole round had no faults (round 33's symbolized
  problem closed).

Round 44 (round 25 hypothesis falsified + new mechanism):
- Measured `actual base_quantum=5 ms` — **round 25's "dynamic quantum mismatch" hypothesis is
  falsified** (the quantum matches the bench's static assumption). The true cause of shortwin
  windows=0 is rerouted: same-phase stalled=11895 (progress==0 samples dominate) → the target
  thread's 1.4ms short slice rarely catches timer ticks (samples land on companion-thread
  slices) → target samples sparse and spanning long gaps; moreover rip_stats's
  short_windows/stalled are **CPU-level totals**, not target-thread-level (eval uses CPU-level
  delta criteria). Next round: sample-landing analysis / target-level counters.

Round 45 (target-level counters confirm):
- Added thread_t-level rip_short_windows/rip_stalled + bench target self-sampling.
- Measured: **target-thread target-level shortwin=0 stalled=0** — samples never land on the
  target's 1.4ms short slices (tick sampling is per slice boundary but the interval ~5ms >
  slice length). Round 44's mechanism confirmed.
- Fix direction finalized: move sampling from ticks to **switch points** (sample the current
  thread at each slice end), tick sampling is inherently blind to short slices; this change
  simultaneously affects the criteria validity of both pollute and shortwin phases — the last
  structural fix in the scheduler's profile.

Round 47 (inline Exit slip-through + TSC timeout + full-chain explanation):
- **Inline Exit slip-through**: round 40's replace_all only replaced the standalone-line form;
  polluter/companion inline `Schedule::Exit(0)` remained → phase 2 kills the bench proc
  (exiting=1) → phase 3 spawns rejected by the round 41 guard (companion=target=NULL) — all
  the earlier "sampling blindness / target never ran" was a chain illusion caused by the spawn
  failure.
- **TSC timeout**: under TCG the polluted phase's CPU saturation freezes the PIT virtual clock
  → wait_done timeout never fires → phase 2 structurally hangs; after switching all phase
  timeouts to TSC baseline (rdtsc × tsc_per_ms) the bench completes within 600s.
- After the fix: mf=823/msl=1056 (target sampled for the first time), dispatch=47 (EEVDF
  high-frequency selection ✓), slice length min=1 max=9ms (preemption truncation visible).

Round 47b (the true stalled mechanism finally appears):
- Loop-count randomization ineffective (stalled 72/57 didn't drop) → the "fast_body RIP
  rehoming" theory falsified.
- **True mechanism**: `CheckPreempt`'s software trap `int SCHED_VEC` has the frame RIP = **the
  fixed address of the trap instruction** (a constant) → deadline-preemption samples always
  have progress 0; only hardware LAPIC tick interrupt frames' RIP is the real execution point.
  The target's slices mostly end via deadline preemption → its samples are almost all
  constant RIP → stalled.
- **Conclusion: switch-point sampling becomes necessary**: the outgoing thread's ctx.rip at
  switch is the real final RIP — move sampling to switch-out + deduplicate with ticks.
  Implementation = next round.

Round 68 (P2-62 disclosure + P3-77):
- D1 (round 10 record): the round 34/35 entries embedded in this section are cross-round
  references (not duplicated paragraphs); the round 28 entry chronologically belongs after
  round 26, currently located at the end of section 9's paragraph — a historical ordering
  blemish, no content lost, to be refiled when tidying up next round.
- ~~Schedule::Sleep + timer wheels (tv1/2/3) are dead code~~ **activated in round 96**:
  PIT::Sleep now calls Schedule::Sleep (timer wheel + THREAD_SLEEPING + Yield, releasing this
  core) once the scheduler is ready (smp_started + thread context available), while the early
  boot probing path keeps busy-waiting. Verification: BUILD_EXIT=0 + golden PASS + QEMU ping 3
  + 0 anomalies.
- ~~Switch-point sampling (round 47b conclusion) still not implemented~~ **round 102 review
  confirms it is implemented**: the sched.cpp Switch path calls riprate_update at every switch
  point (626-628), the shortwin criterion (elapsed < RIP_MIN_SAMPLE_MS → rip_short_windows++)
  and slice-length distribution (rip_min/max_slice_ms) are all live — round 47b's conclusion
  landed in later rounds, documentation lag corrected.
- UAF suspicion narrowed: proc zombie reclamation (DrainProcZombieList) frees FDMan/pagemap
  without global synchronization, and FDMan contains a lock table — the most likely source of
  the lock-word UAF; the fix needs a synchronization audit of fd_manager_destroy and the
  reclamation window.
- Round 35 increment: Exit → PROC_KILL path confirmed (the exiting thread itself goes through
  the process-kill path, disabling interrupts + stopping the timer); there is a race window
  between the kill path's fd cleanup and DrainProcZombieList's FDMan release (triggered by
  dense bench Exits, not observed in production loads). Fix design: defer FDMan release until
  after "all threads unlinked", or move fd cleanup inside the kill path's lock. **Production
  soak never triggered this path → priority = medium.**


History: `sched_bench` previously only had the model-test path; the real-kernel hang trilogy
(bootstrap init calling Yield directly / early PIT::Sleep / init busy-wait starving the
same-core thread) has all been located and bypassed (dedicated thread + init doesn't wait +
wrapper starts the network at the end).

## 9. Tuning Constants Quick Reference

| Constant | Value | Meaning |
|---|---|---|
| `SCHED_STEAL_BATCH` | 8 | push/steal batch size per operation |
| `SCHED_STEAL_THROTTLE` | 8 | steal scan throttle |
| `ZOMBIE_RECLAIM_THRESHOLD/BATCH` | 8 / 16 | zombie-thread reclamation trigger point and batch |
| `RIPRATE_AGING_MS` | 50 | rate aging window |
| `RIPADJ_MIN/MAX` | -4 / +4 | direct correction term range |
| `base_quantum` initial | 5 tick | about 5 ms |
