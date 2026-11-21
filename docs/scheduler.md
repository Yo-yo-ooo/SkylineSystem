# Scheduler: deadline keying and RIP rate feedback

Source: `kernel/src/arch/x86_64/schedule/sched.cpp`, `task.cpp`, `timer.cpp`, `syscall/`.

This describes what the scheduler does, and — where it matters — what it deliberately doesn't.

## 1. Structure

Each CPU has a red-black tree run queue keyed by virtual deadline. On enqueue,
`deadline = vruntime + base_quantum`, and the tree is ordered by deadline. Since deadline is
vruntime plus a constant, ordering by deadline and ordering by vruntime are the same order; the key
field itself is the deadline. Nodes carry a `min_vruntime_subtree` augmentation, and the comparison
points prefetch (`PREFETCH_R`).

Threads get a base slice from `sched_prio_to_weight[16]`. `vruntime` advances by
real elapsed milliseconds × 1024 / weight, and `avg_vruntime` advances at the load-weighted rate.

`Pick()` only looks at `vruntime` and `avg_vruntime`: it descends along the branch that holds an
eligible node and takes the minimum.

## 2. RIP progress feedback

Every tick samples the current thread's RIP progress and pushes it through a Q10 fixed-point
dual-channel EWMA, producing a fast/slow multiplier `mult` clamped to [0.25x, 2.5x] plus a signed
correction `adj` of ±4 ticks. Together they set the thread's slice length:

```text
eff_quantum = (base × fused_mult >> 10) + adj
```

That value is used in exactly one place: the LAPIC oneshot timer duration, the `get_dynamic_quantum()`
call site in `sched.cpp`.

What it does not touch:

- `Pick()` never sees `mult` or `adj`; selection stays vruntime-decided.
- `vruntime` is charged by real time and is not scaled by the multiplier.
- So the feedback shapes preemption rhythm and interrupt latency — busy-waiting threads get
  interrupted more often, progressing threads get longer uninterrupted runs — and does not change CPU
  share. Fairness stays with vruntime.

## 3. How much EEVDF is actually here

- `calibrate_and_set_deadline()` clamps `vruntime` into `[avg−q, avg+2q]` on every enqueue, then sets
  `deadline = vruntime + base_quantum`.
- The slice is weight-scaled: `base_quantum` is multiplied through `sched_prio_to_weight[priority]`
  before becoming the deadline's constant term.
- That makes this a subset of the EEVDF structure, not the full thing. README and this file both say
  so.

## 4. Dynamic base quantum and aging

`dynamic_adjust_quantum()` moves `base_quantum` (clamped to [2,15] ticks) every 100ms based on idle
ratio and context-switch count.

On the feedback side:

- 50ms without a sample makes `mult` decay toward 1.0 in 1/16 steps and `adj` toward 0. The source
  comments record the old bug here: `(x+7)>>4` is always 0 for |adj| ≤ 8.
- Observations above 16x are clamped to 16x.
- New threads with `mult == 0` are treated as 1.0.

## 5. SMP load balancing

- Active push (`TryPush`): when this core has surplus and at least two threads, find the lightest
  target by total weight, preferring the same SIMD feature mask. Note that `cpu_simd_mask()` is an
  instruction-set bitmap, not SMT topology — the kernel keeps no APIC-ID or core-sibling data. Locks
  are taken in `cpu.id` order to avoid ABBA, batches are `SCHED_STEAL_BATCH=8`, and if the target is
  in `hlt` an IPI wakeup goes out immediately after the push.
- Lazy steal (`StealThread`): scans once every 8 throttles, same mask first, then any core.
  `spin_trylock` caps at 100 attempts. After stealing, the thread is marked `THREAD_TRANSFER`, then
  the locks are swapped and it's enqueued.

## 6. Preemption and context switch

`Schedule::Switch()` (SCHED_VEC ISR) stops the LAPIC timer first, and EOI still happens when
`this_cpu == nullptr`. APs re-arm in the early window before returning. If `preempt_count > 1`, the
flags stay set and only the oneshot is rearranged. Voluntary `yield()` takes a separate
`yield_request_flags` path.

Known defect, unfixed: `CheckPreempt()` has no callers anywhere in the tree, so re-triggering after
`preempt_count` reaches zero waits for the next timer tick.

## 7. New-thread accounting

New threads used to leave `last_run_time` uninitialized at every creation site, so the first
`delta` equalled uptime and launched vruntime upward. Creation sites now initialise it, with a
defensive `== 0` check at the accounting point. Every site assigns after `memset`, so behaviour is
deterministic.

## 8. What sched_bench actually found

The benchmark now runs end to end on the real kernel: QEMU, one dedicated thread on cpu 1, network
stack delayed so the machine stays quiet.

| Phase | Result |
|---|---|
| Step (RIP feedback convergence) | t90=4ms, no overshoot |
| Pollution resistance | failed at first: base=[41,2057], mult=[785,2211] |
| Short window | failed at first: windows=0, stalled=5 |
| Oscillation hysteresis | w=[64,64], toggles=0, hysteresis holds |

The two failures turned out not to be one bug but a chain of them, each one hiding the next:

- Short windows were never detected. First hypothesis: `dynamic_adjust_quantum` had moved the
  quantum away from the bench's static assumption. Measurement said `actual base_quantum = 5 ms`,
  which falsified it. The real cause: sampling ran on timer ticks, and the target's 1.4ms slices
  rarely contain a tick, so samples landed on the companion thread's slices. Adding thread-level
  `rip_short_windows`/`rip_stalled` counters and target self-sampling confirmed target-level
  shortwin=0, stalled=0.
- A second sampling problem sat underneath it. `CheckPreempt()`'s software trap `int SCHED_VEC`
  leaves frame RIP at the trap instruction's fixed address, so deadline-preemption samples always
  report progress 0. Only hardware LAPIC ticks carry the real RIP. Since deadline preemption ends
  most slices, stalled counts were inflated. Fix: sample at switch-out, where the outgoing thread's
  `ctx.rip` is the genuine final RIP, and deduplicate against ticks.
- A spawn failure imitated "the target never ran". An earlier `replace_all` only caught the
  standalone-line form of `Schedule::Exit(0)`; the inline calls in polluter and companion threads
  survived. Phase 2 killed the bench process, the exiting guard rejected phase 3's spawn, and every
  downstream measurement was reading a target that didn't exist. All phase threads now idle-spin
  instead of exiting, and `NewKernelThreadEx`/`NewThread` fail explicitly when the process is
  exiting.
- A UAF, symbolized rather than guessed. The exit-path page fault reported RIP at
  `atomic_test_and_set`, CR2=0xFFFF90000211D044 — a lock word inside a kmalloc object, freed while
  another thread still spun on it. Cause: phase threads exiting killed the bench process, and later
  phases spawned into the frame. Closing that also closed the earlier suspicion about
  `DrainProcZombieList` freeing FDMan and pagemap without global synchronisation.
- Pollution oscillation: fixed enough to pass. The denominator was first wrong (`exec_ms`
  spanned other threads' slices), then corrected to the thread's own slice length. Short slices
  truncated by deadline preemption still bias `obs_rate` low, so sampling moved to accumulator
  windows: RIP delta and duration accumulate over 4ms of the thread's own runtime, and per-slice
  noise averages out. After the fix the base phase passes with base=[1067,1309], max deviation
  under 25%; the mult phase [962,1463] still overshoots its [820,1230] bound and needs in-window
  instrumentation.
- The PIT clock froze under saturation. In the polluted phase, TCG CPU saturation stopped the
  PIT virtual clock, so `wait_done` timeouts never fired and phase 2 hung structurally. All phase
  timeouts moved to a TSC baseline (rdtsc × tsc_per_ms) and the bench finishes inside 600s.
  `sched_calibrate_tsc()` came out of the same work: PIT baseline, busy-wait with a millisecond-scale
  guard, because the early version hung at boot when the guard was large and the PIT wasn't running
  yet. It publishes `sched_tsc_per_ms` as a strong symbol.
- Timer wheels came alive. `Schedule::Sleep` plus the tv1/2/3 wheels used to be dead code.
  `PIT::Sleep` now calls into them once the scheduler is ready, while early boot keeps busy-waiting.
- Sleep now works on APs. The wheels used to be advanced only by `Schedule::Tick()` from the PIT IRQ,
  which runs on the BSP; a thread that slept on an AP never got a tick and stayed asleep forever
  (symptom: the desktop pointer froze while clicks still worked, because the cursor thread had been
  stolen to an AP). `Schedule::Switch()` now calls `TimerTickWheels()` on every SCHED_VEC, and idle
  re-arms a 1 ms LAPIC tick when `TimerPending()` reports a sleeper, so sleep wakeup latency is
  ~1 ms instead of "never" (or one full quantum at best).

Final state of the round: target sampled (mf=823/msl=1056), dispatch=47 with high-frequency EEVDF
selection, slice lengths min=1 max=9ms so preemption truncation is visible, and zero anomalies.

Two things stay on the list: the mult-phase overshoot above, and the exit/reclaim race between the
kill path's fd cleanup and `DrainProcZombieList`'s FDMan release. The latter only fires under dense
bench exits and never showed up in production soaks, so it's medium priority.

`sched_bench` used to have only the model path. The real-kernel hangs that blocked it — bootstrap init
calling `Yield` directly, early `PIT::Sleep`, and the init busy-wait starving a same-core thread —
are all located and bypassed: dedicated thread, init doesn't wait, and the wrapper starts the network
at the end.

## 9. Constants

| Constant | Value | Meaning |
|---|---|---|
| `SCHED_STEAL_BATCH` | 8 | push/steal batch size |
| `SCHED_STEAL_THROTTLE` | 8 | steal scan throttle |
| `ZOMBIE_RECLAIM_THRESHOLD/BATCH` | 8 / 16 | zombie thread reclamation trigger and batch |
| `RIPRATE_AGING_MS` | 50 | rate aging window |
| `RIPADJ_MIN/MAX` | -4 / +4 | direct correction range |
| `base_quantum` initial | 5 tick | about 5 ms |
