# Kernel Core Components Stability/Performance Test Report

> Test methodology: move the kernel's **real source** (`kernel/src/mem/heap.cpp` etc.) to a
> WSL/Linux host, replacing only hardware-related dependencies (page allocation, spinlock,
> logging, CLI instructions) with equivalent shims (see `tests/common/`, checklist in
> `tests/README.md`), then run **randomized-parameter chaos tests**, **timed stress tests**,
> and **minimal deterministic regressions**.
> All numbers are produced by the programs under `tests/` on Ubuntu-24.04 / g++14 / -O2 /
> **single-threaded**; `cd tests && bash ci.sh` reproduces everything in one command; seeds are
> hardcoded (slub `0x9e3779b97f4a7c15`, fc `0x2545f4914f6cdd1d`, sched `0xdeadbeefcafebabe`),
> failures are replayable.
> **Conclusion boundary**: single-threaded, no interrupt preemption, libc pages — excludes SMP
> concurrency semantics (see §4).

## Reproduction

```bash
cd tests && bash ci.sh         # slub + slub-poison + fc + fc-reg + sched full suite
```

## 1. SLUB / SLAB Kernel Heap (real source: kernel/src/mem/heap.cpp)

### 1.1 Chaos Test (Randomized Parameters)

- Mode: 2 million random operations per phase × 2 phases; 45% `kmalloc` (1..4096 B canary
  filled), 35% random free, 10% `krealloc` (1..8192 B, verify old content preserved), 10%
  canary spot checks.
- **Results (2026-09-27)**:

| Phase | Operations | Canary spot checks | Failures | Live pages |
|---|---|---|---|---|
| phase 1 | 2,000,000 | 217,075 | **0** | 17 |
| phase 2 | 2,000,000 | 434,781 | **0** | 17 |

- **Zero failures, equal page counts across phases** (17 pages = allocator steady-state page
  pool: SLUB per-size-class active slabs + SLAB page pool retention, `dump_live_pages()` can
  list each page's allocation site; two-phase delta = 0 means no leak).

### 1.2 Boundary/Semantics/Negative Tests (new this round)

| Test | Result |
|---|---|
| 25 sizes (0..65536B) ×32: all non-null, no duplicate addresses, **all 16-aligned** | PASS |
| krealloc shrink/grow/large shrink/same size: content preserved; krealloc(ptr,0)=NULL; krealloc(NULL,n) | PASS |
| kcalloc multiplication wraparound (1<<40 × 1<<12 etc.) | all rejected (NULL) |
| OOM fault injection (page allocation failure): kmalloc(8192)→NULL, krealloc OOM keeps old block, cold-cache SLUB::Alloc→NULL, recovers after removal | PASS |
| Double free → SIGABRT (verified in forked child process) | ACTIVE |
| **Redzone out-of-bounds write** (`-DSLAB_DEBUG_POISON` build, SLAB::Alloc(512) writes p+512 destroying magic → Free detects) | **ACTIVE** |

### 1.3 Latency Distribution (128B class, rdtsc per op, TSC frequency calibrated)

| Load | P50 | P95 | P99 | max |
|---|---|---|---|---|
| No pressure | 91 ns | 96 ns | 131 ns | 11.7 ms (first round: vector growth + first-touch page faults) |
| High load (100k live 256B blocks) | 90 ns | — | 130 ns | 157 µs |
| **Second round (hot cache, verifying max reproduction)** | 91 ns | — | 131 ns | **293 µs (reproduced)** |

**max reproduction check (located)**: second-round slow-path count = **0 times** (128B slab
already hot) yet ~160 outliers >10µs (max ~1.6ms) still appeared → **root cause = host thread
preemption** (rdtsc wall clock includes preempted time, a test-methodology limitation), **not
an allocator defect**; the allocator's own P50/P99 are unaffected.

### 1.4 Per-Size-Class Throughput (op = kmalloc+kfree pair; batch timing, 50k warmup, sink anti-elimination, 3 runs)

| Class | 16B | 32B | 64B | 128B | 256B | 512B | 1024B | krealloc 64→128 |
|---|---|---|---|---|---|---|---|---|
| M ops/s | 12.44 | 12.50 | 12.42 | 12.47 | 12.30 | 12.52 | 12.47 | 5.98 |

3-run range 11.45-12.52 M ops/s. **Note**: the previously reported 8.8M ops/s included one
steady_clock call per iteration with a different op definition; deprecated, this table prevails.
**The "same order of magnitude as same-machine glibc / not a bottleneck" conclusion has been
withdrawn**: the glibc comparison was never measured, and throughput ≠ bottleneck conclusion
(needs kernel-side profile, see `docs/stability-audit.md` §5).

### 1.4b Multithreaded Concurrency (slub-mt: 4 host threads = 4 virtual CPUs)

| Phase | Result |
|---|---|
| A no contention (independent chaos, 500k ops each) | 0 residual 0 failures |
| B cross-thread free (ring passing, 200k ops each) | 0 residual 0 failures (real cross-CPU free path + lock contention) |
| C same-cache contention (4 threads hammer 128B, 5s) | **10.70M ops/s** (single-threaded baseline 12.4M, -14%) |
| TSAN | functional failures=0; **known theoretical race reports exist** (cross-CPU free tail / reclaimed slab read windows, caused no functional failure in chaos, see audit §0'') |

### 1.5 Fragmentation

Internal fragmentation rate (class-granularity overhead) = 130.7% (mainly from >1024B
large-page granularity: 1025B→4096B); **10 rounds × 50k-op chaos trend: page count 5354 →
5354 completely flat, each round largest_free ≥ 4MB** — steady state without growth, no
fragmentation degradation; SLUB intra-class (≤1024) granularity loss is low. This is an
**allocation-granularity characteristic**, not a leak (proven by page-count conservation +
large-block availability).

### 1.5b Redzone Negative Tests (slub-poison standalone `-DSLAB_DEBUG_POISON` build)

| Test | Result |
|---|---|
| oob+64 / +128 / +512 / +1024 (write p+usable destroying the tail redzone → Free detects SIGABRT) | **4/4 ACTIVE** |
| oob-8 (front out-of-bounds) | **N/A** — current layout has only a tail redzone (`obj_size = usable + 8B redzone`), no head redzone, truthfully recorded as a known limitation |

### 1.6 Conclusion (downgraded)

> Under a **single-threaded, no-interrupt-preemption, libc-page shim** environment, SLUB/SLAB's
> algorithmic correctness within the covered scope, memory safety (incl. negative detection),
> boundary semantics, and OOM behavior perform well; SMP concurrency, real interrupt
> preemption, and long-term fragmentation trends are not yet verified. **Not stated as
> "SMP safe" or unconditionally "stable".**

## 2. File Cache (fc.cpp + art.c) — 3 memory-safety defects located and fixed, all pass

**Method**: actually compile `kernel/src/fs/fc.cpp` + `kernel/src/klib/algorithm/art.c` +
`heap.cpp` (only the shim layer replaced), 4 per-CPU cache instances, 200 logical file keys,
400k random operations (promote/get/put/invalidate/idle_handler weighted), with canary content
verification, per-operation ART key-value consistency inspection, generational double-free
tracking, ASAN poisoning, GDB hardware memory watchpoints.

### 2.1 Defects Found (all "invisible to static review, only chaos testing can force out")

| # | Location | Defect | Consequence | Fix |
|---|---|---|---|---|
| 1 | `art.c` 7 sites of `key[depth]` | vendored libart **lost the 0-terminator convention** during the recursion→iteration refactor; when a new key is a true prefix of an existing leaf, `depth==key_len` out-of-bounds read, child index depends on allocator residual bytes | tree structure corruption (visible in traversal / unreachable in search) → cache all-miss | added terminator guard to all `key[depth]` reads (search/insert split/recursive insert/delete/iterate) |
| 2 | `fc.cpp` promote exists branch | old entry's inline capacity allocated per the **original data_len**; reusing larger data makes `__memcpy(exist->inline_data, data, data_len)` overflow directly | out-of-bounds write smashes adjacent entries → memory corruption (GDB watchpoint caught in the act, fc.cpp:1049) | verify `SLUB::TryGetSize(exist)` capacity before reuse, if insufficient detach the old entry and take the new-entry path |
| 3 | promote -4 path / put / pick_and_unlink | on post-insert validation failure `kfree` **the entry still in ART**; pending branch re-unlinks and frees the already-detached entry | ART dangling value, double free corrupts LRU → **the root cause of the actually measured QEMU #GP** | idempotent `art_delete` + insert retry; `art_delete`==NULL means skip; `fc_lru_remove` made idempotent |

### 2.2 Post-Fix Full Results (2026-09-27, g++14, -O2)

| Item | Result |
|---|---|
| Chaos **2,000,000** ops (500 keys Zipf α=1.1, 70% clean / 30% dirty writes) | **failures=0, tree_broken=0** (before the fix tree corruption at op 565 always, OOB at op 935 always) |
| Hit rate (relaxed limits 1MB/2MB) | get-class ops **≈87% hit** (local 29% + cross-core migration 519k times); **15.3% under the 64KB pressure soft limit** — confirmed as a config artifact of migrations rejected by the soft limit (`smoothed_cache_bytes > soft_limit → continue`), not a defect |
| evictions=0 explanation | `file_cache_should_evict` protects entries with "frequency ≥ mean"; under Zipf/uniform loads almost everything is protected → eviction decision never triggers (mechanism confirmed by source reading) |
| **Mixed-load overall throughput + latency (real weights, 3-second batches)** | **0.607M ops/s**; get hit rate 81.7%; get latency P50=583ns P95=2.62µs P99=3.62µs max=2.67ms |
| **Multithreaded concurrency (fc-mt: 4 threads × 4 instances × 300k ops, 30% dirty-write cross-instance broadcast + full-time inspection)** | **failures=0** (TSAN known theoretical races see audit §0'') |
| Double-free tracking / destroy | 0 true double frees / no crash |
| **Three minimal regressions (tests/fc/regression.cpp)** | test_art_true_prefix_key / test_promote_inline_capacity_reuse / test_insert_fail_no_dangling **all PASS** |
| Per-path throughput | get-hit 0.31M / get-miss 7.37M / promote 0.16M / invalidate 11.96M / idle_handler 1.59M ops/s (op definition in tests/README) |

**Conclusion downgrade**: the above are **single-threaded host** conclusions. "All pass" is
restricted to "the suites listed in this table pass"; SMP concurrency (multithreaded per-CPU +
global LRU interaction) and QEMU real load remain to be filled (audit §2 F5).

## 3. Scheduler (Ledger-Model Simulation — a faithful replica of the real source's EEVDF skeleton)

**Method (honest declaration)**: `sched.cpp` is deeply coupled with LAPIC/PIT/thread context
and cannot be compiled directly on the host. This simulation **replicates sched.cpp's ledger
formulas and constants line by line** (every constant/formula is commented against source line
numbers: `RIPRATE_*` sched.cpp:39-69, `get_dynamic_quantum` :205-225, RIP sampling/clamping
:410-467, aging :311-335, `calibrate_and_set_deadline` :588-605, `thread_rb_cmp`/`Pick`
:574-582/:881-919, wakeup preemption :1220-1238, vruntime/avg accounting :1010-1031), the run
queue is a linear-scan substitute (the kernel uses a deadline-sorted red-black tree + subtree
augmentation, O(log n); model O(n), selection result equivalent). Additionally there is a Linux
**full EEVDF reference** model compared side by side.

**Qualitative (corrected via line-by-line source reading)**: the real source's **selection
logic is already an EEVDF skeleton**, not "CFS leftmost vruntime" — ① the run queue is sorted
by **deadline** (`thread_rb_cmp` :574-582); ② the **eligible set** = vruntime ≤ avg (subtree
min_vruntime augmentation makes "subtree contains eligible" an O(1) check, :897-919 descent
search); ③ selection = **min deadline among eligible** (leftmost fallback when the whole tree
has no eligible, :889-894); ④ **wakeup preemption** (:1220-1230): waker eligible and smaller
deadline → interrupt, but **no interruption when the current remaining slice < ¼
base_quantum** (protect_slice variant, saving context switches); ⑤ calibrate's **lag clamping**
(vruntime ∈ [avg−slice, avg+2·slice], :591-602); ⑥ run quantum **scaled by weight**
(`base_quantum×weight/1024`, :212-213) + **per-thread `custom_quantum` override** (the
sched_setattr-slice equivalent interface already exists, :209-211).

**Gaps vs Linux full EEVDF (closed, 2026-09-27)**: the two former substantive differences are
closed in the kernel — ① **vlag accounting**: sleep dequeue saves weighted lag
`(avg−vruntime)·weight`, wakeup places at `vruntime = avg − vlag/weight` then symmetric
clamping (isomorphic to Linux `update_entity_lag`/`place_entity`, see `timer.cpp` Sleep,
`calibrate_and_set_deadline`); ② **deadline protection**: not extended until vruntime passes
the old deadline (Linux `update_deadline` semantics, an unexhausted slice avoids immediate
preemption). Note: this kernel's slice is already weight-scaled (`get_dynamic_quantum`), one
full slice's vruntime consumption is always base_quantum, so `deadline = vruntime + slice` and
Linux's `ve + slice/w` are **isomorphic** in their respective units; the earlier comment about
"offset not weight-scaled" (:589-591) has been corrected along with the implementation.
Residual differences: lag clamp thresholds are constants ±slice/±2·slice (Linux uses
latency-scale thresholds); no task_group hierarchy (does not affect the single-queue
conclusion).
**Deviation magnitude vs real sched.cpp: not quantified** (model throughput and wakeup latency
are model behaviors, and do not constitute performance conclusions about the real kernel).

### 3.1 Fairness (weights 1:1:2:4, 200k ticks)

| Thread | Weight | Measured share | Expected | vruntime |
|---|---|---|---|---|
| 0 | 1 | 0.125 | 0.125 | 93,513,728 |
| 1 | 1 | 0.125 | 0.125 | 93,512,704 |
| 2 | 2 | 0.250 | 0.250 | 93,513,728 |
| 3 | 4 | 0.500 | 0.500 | 93,513,984 |

vruntime max deviation **0.0056%** (real-source replica, slightly looser convergence with
deadline protection) / **0.0000%** (full EEVDF reference).
**Downgraded statement**: this is share consistency under "ledger model, single queue, no
sleep/wakeup", not a "strict verification" of the real scheduler.

### 3.2 RIP Rate Feedback (4 busy-wait + 1 interactive, on/off comparison)

| Mode | Busy-wait avg quantum | Interactive avg quantum |
|---|---|---|
| Feedback off | 3.00 tick | 6.00 tick |
| Feedback on | **1.06 tick** (pressed near the 0.25x lower bound) | **19.45 tick** (stretched to ~4x) |

The time-slice shaping mechanism is clearly visible. **Note**: RIP only shapes slice length and
does not change shares (consistent with scheduler.md); long quanta make interactive threads'
vruntime spike short-term and reduce pick frequency — the real kernel's calibration clamping
and clock advance provide the fallback; this tension has been truthfully recorded (model
behavior, not a real-kernel conclusion).

### 3.3 Random spawn/exit Chaos

200,000 random spawn (weights 1-16, three behavior types)/exit/tick mixes:
**0 failures**, bounded-vruntime invariant holds.

### 3.4 Sleep/Wakeup (exponential sleep mean 20 ticks, incl. real wakeup preemption mechanism)

| Mode | Wakeup→run max latency |
|---|---|
| Real-source replica (vlag placement + wakeup preemption + remaining slice ≥ ¼ protection + deadline protection + **eligible budget truncation**) | **75 ticks** (144 ticks before improvement) |
| Linux full EEVDF reference (eligible includes +slice slack) | 7 ticks |

Conclusion: the pre-improvement 144-tick root cause = RIP-stretched quanta make threads
"overshoot" (fall asleep with negative lag, wakeup waits for avg to catch up); **eligible
budget truncation** (a thread runs at most until vruntime catches avg, eligible ends → slice
ends) brought it down to 75 ticks (-48%). The remaining 75 ticks were confirmed by model
experiments to be a **structural cost** (deadline ties broken by id tiebreak + post-budget-
truncation rotation rhythm): the two candidate adjustments of lag-clamp latency scaling and
preemption suppression ⅛ both yielded nothing, not ported into the kernel.

### 3.5 Fairness (after this round's improvements)

**Eligible budget truncation** simultaneously pressed the fairness max deviation from 0.0056%
to **0.0000%** (all threads park precisely at avg), and reduced RIP-long-quantum pick
starvation from 70 to **5**. Accompanying changes: RIP cap 4× → 2.5× (less "overshoot"),
wakeup lag-clamp band widened to 4×slice (Linux place_entity's latency scale), vlag division
rounded (short sleeps keep lag credit).

### 3.6 Stress

Scheduler core loop (16 threads): ≈3.6M ticks/s (model; incl. per-unit-step wakeup checks and
budget truncation, the number only represents model implementation cost, not real-kernel
throughput).

## 4. Methodology Boundaries (Honest)

- **SLUB/SLAB and file cache**: what is tested is the **real kernel source** (only hardware
  dependencies shimmed), but environmental factors differ from the kernel: no interrupt
  preemption, single-threaded chaos (spinlock degenerates to no contention), pages provided by
  libc. What is proven is the components' **algorithmic correctness, memory safety, and
  single-core overhead**; **SMP concurrency correctness is not verified by this suite**.
- **Scheduler**: a **faithful replica model** of `sched.cpp`'s ledger formulas and constants
  checked line by line (line numbers in the §3 intro) (mode 0: the real source's EEVDF skeleton
  — deadline-tree selection / wakeup preemption / slice protection / lag clamping /
  weight-scaled quantum; mode 1: Linux full EEVDF reference), with the run queue as a
  linear-scan substitute (the kernel uses a deadline red-black tree + subtree augmentation) —
  not compiling `sched.cpp` itself.
- **QEMU real kernel (upgraded)**: `qemu-system-x86_64` (Windows host, **TCG without KVM**),
  `-m 2G`, full parameters (incl. disk.img/network/sound) + **`-debugcon` capturing the E9
  port** (kernel panic messages go out via E9, not serial — the previous serial grep would miss
  them): **core-count sweep smp 1/2/4 × 45s each without anomalies; smp 4 long stability 30
  minutes without anomalies**. **This round's QEMU upgrade actually caught one real
  regression**: the tagged free-chain change caused `slub_stack_pop` #GP (located via
  addr2line), reverted and re-verified all green. This is **smoke/long stability, not a proof
  of SMP correctness**.
- All test programs live under `tests/`, one-command reproduction: `cd tests && bash ci.sh`.
