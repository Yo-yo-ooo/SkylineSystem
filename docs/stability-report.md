# Kernel components: stability and performance measurements

How the numbers in this file were produced: the kernel's real sources (`kernel/src/mem/heap.cpp` and
friends) are moved to a WSL/Linux host, and only hardware-dependent pieces — page allocation, spinlocks, logging,
`cli` — are replaced with shims (`tests/common/`, list in `tests/README.md`). Then the suites run
randomised chaos, timed stress and a few minimal deterministic regressions.

Everything below comes from `tests/` on Ubuntu 24.04, g++ 14, `-O2`, single-threaded. `cd tests &&
bash ci.sh` reproduces it in one command. Seeds are hardcoded (slub `0x9e3779b97f4a7c15`, fc
`0x2545f4914f6cdd1d`, sched `0xdeadbeefcafebabe`), so failures replay.

Scope: single-threaded, no interrupt preemption, pages from libc. SMP concurrency semantics are outside
this measurement, see §4.

```bash
cd tests && bash ci.sh         # slub + slub-poison + fc + fc-reg + sched
```

## 1. SLUB / SLAB heap

Real source: `kernel/src/mem/heap.cpp`.

### 1.1 Chaos, randomised parameters

Two phases of 2 million random operations: 45% `kmalloc` (1..4096 B, canary filled), 35% random free,
10% `krealloc` (1..8192 B, old content verified), 10% canary spot checks. Measured 2026-09-27.

| Phase | Operations | Canary checks | Failures | Live pages |
|---|---|---|---|---|
| phase 1 | 2,000,000 | 217,075 | 0 | 17 |
| phase 2 | 2,000,000 | 434,781 | 0 | 17 |

Zero failures, and the page count is identical across phases. Those 17 pages are the steady-state pool:
SLUB active slabs per size class plus the SLAB page pool. `dump_live_pages()` prints each page's
allocation site. A two-phase delta of zero means no leak.

### 1.2 Boundary, semantics, negative tests

| Test | Result |
|---|---|
| 25 sizes (0..65536B) ×32: all non-null, no duplicate addresses, all 16-aligned | PASS |
| krealloc shrink/grow/large shrink/same size: content preserved; `krealloc(ptr,0)=NULL`; `krealloc(NULL,n)` | PASS |
| kcalloc multiplication wraparound (`1<<40 × 1<<12` and friends) | all rejected (NULL) |
| OOM injection on page allocation: `kmalloc(8192)`→NULL, krealloc keeps the old block, cold-cache `SLUB::Alloc`→NULL, recovers after removal | PASS |
| Double free → SIGABRT, verified in a forked child | ACTIVE |
| Redzone out-of-bounds write (`-DSLAB_DEBUG_POISON`, `SLAB::Alloc(512)` writes `p+512` over the magic) | ACTIVE |

### 1.3 Latency, 128B class, rdtsc per op, TSC calibrated

| Load | P50 | P95 | P99 | max |
|---|---|---|---|---|
| No pressure | 91 ns | 96 ns | 131 ns | 11.7 ms (first round: vector growth and first-touch page faults) |
| High load (100k live 256B blocks) | 90 ns | — | 130 ns | 157 µs |
| Second round (hot cache) | 91 ns | — | 131 ns | 293 µs (reproduced) |

The tail is host thread preemption, not the allocator. Second round showed zero slow-path hits (the
128B slab was already hot) yet ~160 outliers above 10µs, max ~1.6ms. rdtsc measures wall clock, so preempted
time lands in the sample. P50 and P99 of the allocator itself are unaffected.

### 1.4 Per-class throughput

One op = a kmalloc+kfree pair, batch timing, 50k warmup, sink accumulation against dead-code elimination, three runs.

| Class | 16B | 32B | 64B | 128B | 256B | 512B | 1024B | krealloc 64→128 |
|---|---|---|---|---|---|---|---|---|
| M ops/s | 12.44 | 12.50 | 12.42 | 12.47 | 12.30 | 12.52 | 12.47 | 5.98 |

Three-run range 11.45-12.52 M ops/s. The 8.8M figure published earlier came from an op definition that
included one `steady_clock` call per iteration; that one is retired and this table stands. The old
"same order of magnitude as glibc, not a bottleneck" conclusion is withdrawn too: glibc was never measured
on this machine, and throughput alone doesn't prove anything about bottlenecks (that needs a kernel-side
profile, see `stability-audit.md` §5).

### 1.4b Multithreaded (slub-mt, 4 host threads = 4 virtual CPUs)

| Phase | Result |
|---|---|
| A, no contention (independent chaos, 500k ops each) | 0 residual, 0 failures |
| B, cross-thread free (ring passing, 200k ops each) | 0 residual, 0 failures; exercises the real cross-CPU free path and lock contention |
| C, same-cache contention (4 threads on 128B for 5s) | 10.70M ops/s against a 12.4M single-thread baseline, -14% |
| TSAN | no functional failures; the theoretical race reports are known (cross-CPU free tail, reclaimed slab read window) and never fired in chaos, see audit §0 |

### 1.5 Fragmentation

Internal fragmentation at class granularity is 130.7%, mostly the >1024B path (1025B → 4096B). Over ten
rounds of 50k-op chaos the page count stayed flat at 5354 → 5354, and each round kept at least 4MB
largest-free. That is a steady state with no growth and no fragmentation trend; SLUB's loss inside its
own classes (≤1024) is small. It's an allocation-granularity characteristic, not a leak — page-count
conservation plus available large blocks is the evidence.

### 1.5b Redzone negatives (slub-poison, standalone `-DSLAB_DEBUG_POISON`)

| Test | Result |
|---|---|
| oob +64 / +128 / +512 / +1024, writing `p+usable` over the tail redzone | 4/4 ACTIVE |
| oob -8, front out-of-bounds | N/A: the current layout only has a tail redzone (`obj_size = usable + 8B`). No head redzone exists; recorded as a limitation |

### 1.6 What this does and does not show

Inside a single-threaded shim with no interrupt preemption and libc-provided pages, SLUB/SLAB is
correct in the covered scope: memory safety including negative detection, boundary semantics, OOM
behaviour. SMP concurrency, real preemption and long-run fragmentation are not verified by this suite
and are not claimed.

## 2. File cache (fc.cpp + art.c): three memory-safety defects, all fixed

`kernel/modules/common/fs/fc.cpp` and `kernel/src/klib/algorithm/art.c` are compiled for
real (only the shim layer is replaced), along with `heap.cpp`:
four per-CPU cache instances, 200 logical file keys, 400k random operations weighted across
promote/get/put/invalidate/idle_handler, with canary content checks, per-operation ART consistency
inspection, generational double-free tracking, ASAN poisoning and GDB hardware watchpoints.

### 2.1 The defects

None of these were visible to static review; chaos testing forced them out.

| # | Location | Defect | Consequence | Fix |
|---|---|---|---|---|
| 1 | `art.c`, seven `key[depth]` sites | the vendored libart lost the 0-terminator convention in the recursion→iteration refactor; when a new key is a true prefix of an existing leaf, `depth==key_len` reads out of bounds and the child index depends on allocator residue | tree corruption: traversal sees nodes that search can't reach, cache misses everything | terminator guards on every `key[depth]` read (search, insert split, recursive insert, delete, iterate) |
| 2 | `fc.cpp` promote, exists branch | the old entry's inline capacity was allocated for the original `data_len`; reusing it for larger data makes `__memcpy(exist->inline_data, data, data_len)` overflow | out-of-bounds write into adjacent entries; GDB caught it at fc.cpp:1049 | check `SLUB::TryGetSize(exist)` before reuse, and detach the old entry into the new-entry path when it's too small |
| 3 | promote -4 path, put, pick_and_unlink | post-insert validation failure freed an entry that was still in the ART; the pending branch then re-unlinked and freed the already-detached entry | dangling ART value plus a double free corrupting the LRU — this is the root cause of the #GP observed under QEMU | idempotent `art_delete` plus insert retry; `art_delete == NULL` skips; `fc_lru_remove` made idempotent |

### 2.2 Results after the fixes (2026-09-27, g++14, -O2)

| Item | Result |
|---|---|
| Chaos, 2,000,000 ops (500 keys, Zipf α=1.1, 70% clean / 30% dirty writes) | failures=0, tree_broken=0. Before the fix, tree corruption hit at op 565 and OOB at op 935, every run |
| Hit rate, relaxed limits 1MB/2MB | get-class ops ≈87% (local 29%, cross-core migration 519k times). 15.3% under the 64KB soft limit was a configuration artifact: migrations rejected by `smoothed_cache_bytes > soft_limit → continue`, not a defect |
| evictions=0 | `file_cache_should_evict` protects entries with frequency ≥ mean; under Zipf and uniform loads nearly everything qualifies, so the eviction decision never fires. Confirmed by reading the source |
| Mixed load, real weights, 3-second batches | 0.607M ops/s, get hit rate 81.7%, get latency P50=583ns P95=2.62µs P99=3.62µs max=2.67ms |
| Multithreaded (fc-mt, 4 threads × 4 instances × 300k ops, 30% dirty-write cross-instance broadcast with full-time inspection) | failures=0; TSAN's theoretical races are covered in audit §0 |
| Double-free tracking and destroy | 0 true double frees, no crash |
| Three minimal regressions (`tests/fc/regression.cpp`) | `test_art_true_prefix_key`, `test_promote_inline_capacity_reuse`, `test_insert_fail_no_dangling` all PASS |
| Per-path throughput | get-hit 0.31M / get-miss 7.37M / promote 0.16M / invalidate 11.96M / idle_handler 1.59M ops/s (op definitions in `tests/README.md`) |

All of the above is single-threaded host work. "All pass" means "the suites in this table pass". SMP
concurrency — multithreaded per-CPU caches against the global LRU — and real load under QEMU are still
open (audit §2 F5).

## 3. Scheduler: ledger-model simulation

`sched.cpp` is tightly coupled to LAPIC, PIT and thread context, so it can't be compiled on the host.
The model replicates sched.cpp's ledger formulas and constants line by line, each commented against
source line numbers: `RIPRATE_*` sched.cpp:39-69, `get_dynamic_quantum` :205-225, RIP
sampling/clamping :410-467, aging :311-335, `calibrate_and_set_deadline` :588-605,
`thread_rb_cmp`/`Pick` :574-582 / :881-919, wakeup preemption :1220-1238, vruntime/avg accounting
:1010-1031. The run queue is a linear-scan substitute for the kernel's deadline-sorted red-black tree
with subtree augmentation: O(n) instead of O(log n), same selection result. A full Linux EEVDF
reference model runs alongside for comparison.

Reading the source line by line changed the description: selection is already an EEVDF skeleton, not
CFS-style leftmost vruntime. Concretely — the queue sorts by deadline (`thread_rb_cmp` :574-582); the
eligible set is vruntime ≤ avg, and the `min_vruntime_subtree` augmentation makes "subtree holds an
eligible node" an O(1) test (:897-919 descent); selection takes the minimum deadline among eligible
entries, falling back to leftmost when nothing is eligible (:889-894); wakeup preemption (:1220-1230)
interrupts when the waker is eligible with a smaller deadline, unless the current remaining slice is
under ¼ base_quantum; calibrate clamps lag to vruntime ∈ [avg−slice, avg+2·slice] (:591-602); the
quantum is weight-scaled (`base_quantum×weight/1024`, :212-213) with a per-thread `custom_quantum`
override already in place (:209-211).

The two substantive gaps against Linux EEVDF closed in the kernel on 2026-09-27: vlag accounting
(dequeue on sleep saves weighted lag `(avg−vruntime)·weight`, wakeup places at
`vruntime = avg − vlag/weight` then clamps symmetrically, isomorphic to Linux
`update_entity_lag`/`place_entity`, see `timer.cpp` Sleep and `calibrate_and_set_deadline`), and
deadline protection (not extended until vruntime passes the old deadline, i.e. an unexhausted slice
avoids immediate preemption, the same rule as Linux `update_deadline`). This kernel's slice is already weight-scaled, so one full slice always
consumes `base_quantum` of vruntime and `deadline = vruntime + slice` is isomorphic to Linux's
`ve + slice/w`. The old comment claiming the offset wasn't weight-scaled (:589-591) is corrected
along with the implementation.

What remains different: lag clamp thresholds are constants (±slice, ±2·slice) where Linux uses
latency-scale thresholds, and there is no task_group hierarchy, which doesn't affect single-queue
conclusions. How far the model deviates from real `sched.cpp` is not quantified — model throughput and
wakeup latency are model behaviour, not statements about the kernel.

### 3.1 Fairness, weights 1:1:2:4, 200k ticks

| Thread | Weight | Measured share | Expected | vruntime |
|---|---|---|---|---|
| 0 | 1 | 0.125 | 0.125 | 93,513,728 |
| 1 | 1 | 0.125 | 0.125 | 93,512,704 |
| 2 | 2 | 0.250 | 0.250 | 93,513,728 |
| 3 | 4 | 0.500 | 0.500 | 93,513,984 |

vruntime deviates by at most 0.0056% in the source replica (slightly looser with deadline protection)
and 0.0000% in the full EEVDF reference. This is share consistency under a ledger model with a single
queue and no sleep/wakeup, not a verification of the real scheduler.

### 3.2 RIP rate feedback, four busy-wait threads plus one interactive

| Mode | Busy-wait avg quantum | Interactive avg quantum |
|---|---|---|
| Feedback off | 3.00 tick | 6.00 tick |
| Feedback on | 1.06 tick (near the 0.25x floor) | 19.45 tick (stretched to about 4x) |

Slice shaping is visible. RIP only changes slice length and never shares, consistent with
`scheduler.md`. Long quanta do make interactive threads' vruntime spike short-term and cut their pick
frequency; the kernel's calibration clamping and clock advance are the fallback. That tension is
recorded here rather than smoothed over — with the caveat that this is model behaviour.

### 3.3 Random spawn/exit chaos

200,000 mixes of spawn (weights 1-16, three behaviour types), exit and tick: 0 failures, the
bounded-vruntime invariant holds.

### 3.4 Sleep and wakeup, exponential sleep mean 20 ticks, real wakeup preemption included

| Mode | Wakeup→run max latency |
|---|---|
| Source replica (vlag placement, wakeup preemption, remaining slice ≥ ¼ protection, deadline protection, eligible budget truncation) | 75 ticks (144 before the improvements) |
| Linux EEVDF reference, eligible includes +slice slack | 7 ticks |

The 144-tick figure came from RIP-stretched quanta: threads overshot, fell asleep with negative lag,
and wakeup waited for avg to catch up. Eligible budget truncation — a thread runs only until vruntime
catches avg, then the slice ends — cut it to 75, a 48% improvement. Model experiments showed the
remaining 75 ticks are structural: deadline ties broken by id, plus the rotation rhythm after truncation.
Neither candidate adjustment (latency-scaled lag clamp, preemption suppressed to ⅛) changed anything, so
neither was ported.

### 3.5 Fairness after these changes

Budget truncation also drove the fairness deviation from 0.0056% to 0.0000% (threads park exactly at
avg) and cut RIP-long-quantum pick starvation from 70 to 5. Alongside: RIP cap 4× → 2.5×, wakeup
lag-clamp band widened to 4×slice, vlag division rounded so short sleeps keep their credit.

### 3.6 Model cost

The scheduler core loop with 16 threads runs at about 3.6M ticks/s. That includes per-unit-step wakeup
checks and budget truncation; it measures the model implementation, not real kernel throughput.

## 4. What this methodology covers

- SLUB/SLAB and the file cache are real kernel sources with hardware dependencies shimmed, but the
  environment differs: no interrupt preemption, single-threaded chaos (so spinlocks never contend),
  pages from libc. That proves algorithmic correctness, memory safety and single-core overhead. It
  does not verify SMP concurrency.
- The scheduler is a replica of `sched.cpp`'s ledger formulas and constants, checked line by line
  against the source (§3 lists the line numbers), with a linear-scan run queue. `sched.cpp` itself
  is never compiled here.
- QEMU: `qemu-system-x86_64` on a Windows host, TCG without KVM, `-m 2G`, full device set
  (disk.img, network, sound) plus `-debugcon` capturing the E9 port, because kernel panics go out over
  E9 and a serial grep would miss them. CPU count sweep at smp 1/2/4, 45s each, no anomalies; smp 4
  for 30 minutes, no anomalies. That sweep did catch a real regression: the tagged free-chain change
  caused `slub_stack_pop` #GP, found with addr2line, reverted, re-verified green. This is smoke and
  long-run stability, not a proof of SMP correctness.
- Every test lives under `tests/`, reproduced with `cd tests && bash ci.sh`.
