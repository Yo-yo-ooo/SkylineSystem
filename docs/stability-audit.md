# Stability Report Self-Audit: Evidence Gaps, Retest Plans, Conclusion Downgrades and Roadmap

> This file audits `docs/stability-report.md` item by item. Principles:
> ① Strictly distinguish "real kernel source compiled" / "formula-replica model" / "pure simulation";
> ② Single-threaded shim conclusions must not be written as "component stable" or "SMP safe";
> ③ Performance numbers without context are all considered invalid.

## 0'. Retest Progress (Items Landed, 2026-09-27)

| Audit Item | Status | Evidence |
|---|---|---|
| S1-S5 boundary/krealloc/kcalloc wraparound/OOM injection/alignment | ✅ | `tests/slub/main.cpp` all PASS (report §1.2) |
| S2 latency distribution P50/P95/P99 | ✅ | 128B class 2M ops, P50=86ns/P99=120ns (report §1.3) |
| S8 per-class throughput + batch timing + sink | ✅ | 16..1024B seven classes + krealloc (report §1.4) |
| S7 OOM path | ✅ | `vmm_set_fail_all` injection: large block NULL, krealloc keeps old block, cold cache NULL, recovery (§1.2) |
| Redzone out-of-bounds negative test | ✅ | `slub-poison` standalone build (`-DSLAB_DEBUG_POISON`), Free detects ACTIVE |
| S9 live_pages mechanism | ✅ | 17 pages = SLUB class active slabs + SLAB page pool; `dump_live_pages()` prints allocation sites |
| F4 three minimal regressions | ✅ | `tests/fc/regression.cpp`: test_art_true_prefix_key / test_promote_inline_capacity_reuse / test_insert_fail_no_dangling (fault injection) all PASS |
| F1 per-path throughput | ✅ | get-hit/get-miss/promote/invalidate/idle_handler (report §2) |
| F2 hit rate statistics | ✅ | hits/misses/migrations output (report §2) |
| Long chaos | ✅ | 2M ops, 500 keys, soft/hard limits 64KB/256KB (report §2) |
| Scheduler EEVDF reference comparison | ✅ | weighted slice+VD model, fairness/wakeup latency side by side (report §3.1/§3.4) |
| M3 seed printing | ✅ | three suites print seeds at startup |
| M4 CI | ✅ | `tests/ci.sh` full suite (slub/slub-poison/fc/fc-reg/sched) |
| Five overstated claims | ✅ | report has downgraded/corrected all of them (see §5 handling column) |
| **Not completed** | ⏳ | S6/F5 multithreaded concurrency (partially done, see below), QEMU sched trace, before/after fix performance comparison, golden-sample diff, evictions counter trigger-condition verification |

## 0''. SMP Retest Progress (Multithreading + TSAN, 2026-09-27 Round 2)

**Kernel-side fixes ultimately kept** (all verified no-regression by QEMU boot):

| # | Fix | Evidence | Status |
|---|---|---|---|
| 1 | `art.c` NODE4/16 node cache gets spinlock | fc-mt segfault (remove_child16) | ✅ kept |
| 4 | `fc.cpp total_entries` fully `__atomic_*` | TSAN fc.cpp:574/803 | ✅ kept |
| 5 | kernel `spinlock_lock/unlock` unified `extern "C"` | link error | ✅ kept |

**Fixes reverted (important lesson)**:

| Fix | Revert reason | Lesson |
|---|---|---|
| SLUB free-chain 64-bit tagged pointers (anti-ABA) | **QEMU core-count sweep actually #GP**: kernel HHDM addresses (0xFFFF_8xxx) have high 16 bits = 0xFFFF, packing truncated the address → unpacking produced a non-canonical pointer (addr2line located `slub_stack_pop` heap.cpp:851) | Host test addresses < 2^48 masked the problem; **real kernel verification must run immediately after every change** |
| `SLUB::Free` tail state machine lock-in + magic precondition | After reverting tagged, boot still abnormal → reverted together to git HEAD semantics (original reclaim/link structure) | TSAN race was real, but the "fix" changed early boot/interrupt path semantics |
| `slow_alloc`/`Alloc` in-lock accounting refactor | Same as above | Same as above |

**TSAN known reports (unfixed, listed as roadmap)**: cross-CPU free on shared slabs —
`reclaim_slab` vs Free tail reads race, revived path post-unlock accounting window — none of
which caused functional failures in 4-thread chaos (fc-mt/slub-mt failures=0); Linux avoids
this structurally with per-CPU freelists; this kernel's roadmap: per-CPU freelist rework (P2).

**Verification status (closed)**:
- `slub-mt` (4 threads per-CPU + cross-thread free + same-cache contention): **failures=0**; contended throughput 10.7M ops/s (-14%).
- `fc-mt` (4 threads × 4 instances × 300k ops, 30% dirty-write cross-instance broadcast + full-time inspection): **failures=0**.
- **Lesson 2**: the series of "residual crashes" during fc-mt convergence was finally traced to a **harness bug itself** — `alloca` accumulating in a loop blew up the 8MB thread stack; after switching to fixed-size arrays everything is green (unrelated to kernel fixes).
- **QEMU core-count sweep (with disk parameters)**: smp 1/2/4 × 45s each **all without anomalies** (19-20KB serial logs).
- Conclusion: **"single-core algorithm correct + 4-thread shim concurrency passes (functional green); SMP theoretical races listed as known limitations, real kernel passes core-count sweep and 30-minute long stability without anomalies"**.

### 0'''. New Verifications This Round

| Item | Result |
|---|---|
| God-file split | `mem/heap.cpp` (1344 lines) → `slab.cpp`(755) + `slub.cpp`(456) + `heap.cpp`(127, kmalloc glue) + `heap_internal.h`(shared helpers); `SLAB::AllocAligned` declaration added to heap.h; two diagnostic accessors migrated to slab.cpp along with the globals; kernel Makefile auto-globs, no change needed, tests/Makefile `HEAP_SRC` updated. **CI seven suites all green, kernel 0 warnings, QEMU smp 2/4 boot without anomalies** |
| God-file split (cont.) | `schedule/sched.cpp` (1356 lines) → `sched.cpp`(896, core: Pick/calibrate/tick/SMP balance) + `sched_rip.cpp`(482, RIP feedback + quantum layer) + `sched_internal.h`(shared constants/structs/entry points); need_resched/yield flags and prio table stay in the core. **Kernel 0 warnings, QEMU smp 2/4 boot without anomalies**. Still to split: `fs/fc.cpp`(1660, planned split by idle/oscillate), `art.c`(vendored upstream, untouched) |
| God-file split (completed) | `fs/fc.cpp` (1670 lines) → `fc.cpp`(1279, core lifecycle/API) + `fc_idle.cpp`(406, background maintenance/fsync/writeback) + `fc_internal.h`(25, shared 10 functions + 2 globals + stats struct); **CI seven suites all green (fc 2M-op chaos 0 failures), kernel 0 warnings, QEMU smp 2/4 boot without anomalies**. With this, all splittable god-files in this project are done: heap/sched/fc three modules split into 6 new files + 3 internal headers; `art.c`/`ext4.cpp` are vendored upstream code (libart/lwext4), untouched |
| EEVDF remaining-gap experiment | Model tested two candidate adjustments (lag clamping latency scaling `(2+nr)×slice`, preemption suppression ¼→⅛) — **wakeup latency unchanged in both** (75 ticks). The remaining gap = deadline-tie id tiebreak + budget-truncation rotation's **structural cost**, not a tunable parameter; conclusion: don't port ineffective adjustments into the kernel (model and kernel stay consistent at 4×slice + ¼) |
| Scheduler fairness/tail-latency improvements | see report §3.4/§3.5: eligible budget truncation + RIP 2.5× + lag band 4×slice + division rounding → fairness max deviation 0.0000%, wakeup 144→75 ticks, pick starvation 70→5 |
| QEMU 30-minute long stability (smp 4, idle desktop) | no anomalies (note: that round's grep was case-sensitive; discovered afterwards that the E9 port is where panic messages exit; from this round on all QEMU runs add `-debugcon` to capture E9) |
| SLUB latency outlier root cause | **Second round slow path = 0 times** (128B slab already hot), still 158 outliers >10µs → **attributed to host thread preemption** (rdtsc wall clock includes preempted time), not the allocator |
| LSAN (fc-asan + detect_leaks, destructor returns shim page pool) | **zero leak reports** |
| gcov coverage (fc 2M-op chaos + per-path + mixed load, O0) | **fc.cpp line 41.7%** (branch both-directions 29.9%) / **heap.cpp line 32.5%** (branch both-directions 18.1%) / **art.c line 50.6%** (branch both-directions 40.4%) — uncovered branches are potential blind spots (OOM error paths, rare state machines, defensive branches) |

---

## 7. P2 Reinforcement Landing Record

### 7.1 SLUB vs Linux SLUB Qualitative Mechanism Comparison (no numeric comparison)

| Mechanism | This kernel's SLUB | Linux SLUB (6.x) | Difference notes |
|---|---|---|---|
| Fast path | per-CPU active slab + lock-free CAS free chain (tagged pointers anti-ABA) | per-CPU freelist (`slub_percpu_partial`, also lock-free CAS) | Same structural origin; this kernel's tag bits are in the pointer's high 16 bits, Linux uses low 4 bits (PAGE alignment) |
| Slow path | global `c->lock` serialization + partial list refill | per-CPU partial + node partial + same lock idea | Same idea, no node layer |
| Cross-CPU free | shared slab atomic stack (this round fixed ABA/tail state machine) | `__slab_free` goes per-CPU path + slab migration | This kernel allows cross-CPU push (proven TSAN clean) |
| NUMA | **none** (single-node assumption) | per-node kmem_cache_cpu | Explicitly not implemented |
| Memory reclaim | Free tail inuse==0 returns whole page immediately | `slab_free` + periodic drain + `kmem_cache_shrink` | This kernel is aggressive (no reserved pool), fragmentation trend proven stable |
| Page allocator | VMM::Alloc (above buddy) | page allocator (buddy + PCP) | During tests substituted by host page pool |
| Tuning interface | none | sysfs / slabinfo | none |
| CPU partial cap | MAX_CPU static array | per-cpu dynamic | fixed 64 cap |

### 7.2 File Cache Semantic Path Coverage Checklist (the precise boundary of "all pass")

**Before/after fix performance comparison (P1 item)**: the pre-fix binaries for ART terminator
guard/TryGetSize check/idempotent delete were not kept, **so before/after comparison is
impossible**; post-fix absolute numbers have been provided (per-path + mixed load 0.607M
ops/s). The guard cost is one branch comparison per `key[depth]`, not quantified separately —
recorded truthfully as a data gap, no fabricated numbers.

| Path | Kernel implementation | Test coverage |
|---|---|---|
| promote (write promotion) | ✅ | ✅ chaos 40% weight + capacity reuse regression + fault injection |
| get/put (incl. cross-core migration, pin) | ✅ | ✅ 35% weight + strict content verification (after dirty write) |
| invalidate (cross-core broadcast) | ✅ | ✅ 10% weight + post-invalidation consistency check |
| idle_handler/tick (LRU decay, eviction decision) | ✅ | ✅ 15% weight |
| **writeback/dirty-page writeback** | ✅ (wb_cb + dirty-ratio throttling) | ⚠️ only callback stub + throttle trigger path (regression uses is_dirty=false; chaos dirty ratio low) |
| **readahead** | ⚠️ (access_freq==0 cold eviction, no active readahead) | ❌ no active readahead logic to test |
| **truncate/fsync** | ❌ not implemented (non-POSIX OS) | N/A |
| **OOM reclamation** (`fc_try_evict_for_space`) | ✅ | ⚠️ migration rejection path observed under pressure-limit config; active OOM reclamation not separately stress-tested |

### 7.3 QEMU Real-Kernel Load

- **fio/filebench/perf/trace-cmd have no equivalents on this OS** — a hobby OS has no userspace benchmarking toolchain.
  Feasible alternatives (roadmap): in-kernel `sched_bench` serial trace + file-cache self-stress module.
- Done: TCG 30-minute long stability (smp 4) without anomalies + core-count sweep (smp 1/2/4 × 45s each), see §0'''.

### 7.4 Memory Detection Matrix (by module)

| Detection | SLUB | File cache | Scheduler |
|---|---|---|---|
| ASAN (OOB/UAF) | ✅ (slub + redzone standalone build) | ✅ (fc-asan chaos) | N/A (model) |
| LSAN (leaks) | ✅ (**zero leak reports** after destructor returns shim page pool) | ✅ same as left | N/A |
| TSAN (data races) | ⚠️ functional green + known theoretical race reports (§0'') | ⚠️ same as left | N/A (single-threaded model) |
| Valgrind memcheck | ✅ fc-reg: **definitely/indirectly lost = 0**; all 9 warnings are shim `posix_memalign` wrapper (alignment args), 0 invalid accesses to kernel objects | ✅ same as left | N/A |
| gcov coverage | ✅ heap.cpp line 32.5% | ✅ fc.cpp 41.7% / art.c 50.6% | N/A |

---

## 0. Honest Baseline of Existing Tests (audit starting point)

| Fact | Value |
|---|---|
| Test host | WSL2 (Ubuntu-24.04), g++ 14, -O2; **CPU model/core count/pinning/governor not recorded** |
| Threading model | all **single-threaded**; shim spinlock degenerates to no contention |
| Random seeds | hardcoded: slub `0x9E3779B97F4A7C15ULL`, fc `0x2545F4914F6CDD1DULL`, sched `0xDEADBEEFCAFEBABEULL` (deterministic ✓, but not written into the report) |
| ops definition | SLUB stress 1 op = kmalloc+single-byte memset+kfree (**each iteration includes one steady_clock timing call**); named cache 1 op = alloc+free; fc 1 op = kcopy(kmalloc+memcpy)+promote(is_dirty=true, incl. broadcast); sched 1 tick = pick thread + run quantum + accounting |
| QEMU verification | Windows qemu-system-x86_64, `-machine q35 -cpu max`, **TCG (no KVM)**, 2G/4 cores, idle desktop load; latest round is **150 seconds** smoke (early 300-second rounds predate the ART/inline fixes) |
| Performance number sources | 3 runs only for SLUB; fc/sched have no variance / no P50/P99 / no per-class / no warmup description |

---

## 1. SLUB/SLAB Gaps and Retest Plans

### 1.1 Missing Evidence Checklist

| # | Gap | Current state |
|---|---|---|
| S1 | Per-size-class throughput/latency | Only one number for 1..512B mixed, no per-class data |
| S2 | Latency distribution | Only throughput, no P50/P95/P99 |
| S3 | krealloc dedicated tests | 10% weight in chaos, no assertions for shrink-in-place / cross-class growth / realloc(0) / realloc failure |
| S4 | Boundary cases | kmalloc(0)/kmalloc(1)/1024/1025/4096/large-page boundary, kmalloc_aligned, kcalloc overflow (numitems*size wraparound) untested |
| S5 | Fragmentation metrics | Only live_pages; no internal fragmentation rate (Σ(class size-request)/Σrequest), page utilization, peak RSS |
| S6 | Multithreaded per-CPU | Completely missing (cslab magazine and SLUB cpu_active only ever run on cpu id=0) |
| S7 | OOM/fault injection | Path where VMM::Alloc returns NULL never triggered (fc's eviction fallback is a dead letter) |
| S8 | Anti-optimization | Timing loop calls clock every iteration, measurement includes measurement overhead; no sink checksum to prevent dead-code elimination; no -O0/-O2/LTO comparison |
| S9 | Mechanism explanation for live_pages=9 | Report only says "steady-state page pool", never verified what the 9 pages each are (hypothesis: SLAB 7 size classes × 1 page each + metadata + SLUB named-cache descriptor, **unverified**) |
| S10 | CI / golden samples | No CI config, no expected-output snapshots, no regression gate |

### 1.2 Retest Plan (reproducible into tests/slub/)

| Test name | Purpose | Design/variables | Metrics | Expectation | Anti-self-deception |
|---|---|---|---|---|---|
| `test_boundary_sizes` | boundaries don't crash or overlap | size ∈ {0,1,15,16,17,31,32,63,64,127,128,1023,1024,1025,2048,4095,4096,4097,65536}, 100 allocations per size, full uniqueness check | dup=0, all non-null | all pass | fixed seed; checksum write-back + sink print |
| `test_krealloc_shrink_grow` | realloc semantics | 128→64(in-place), 64→4096(cross-class), 4096→8, ptr+0(free returns NULL), NULL+size(equivalent to kmalloc); assert content preserved at each step | min-region content consistency | all consistent | canary mode + uniqueness table |
| `test_kcalloc_overflow` | multiplication wraparound | kcalloc(1<<40, 1<<12) etc., assert returns NULL or safely fails without corrupting writes | no crash, no allocation | rejected | negative test |
| `test_oom_path` | OOM behavior | shim adds `g_fail_next_alloc` switch: Nth VMM::Alloc returns NULL; test kmalloc large block/SLUB slow path/fallback | no crash, correct error code, recoverable afterwards | rejected and recovered | fault-injection switch unit test |
| `fragmentation_steady_state` | fragmentation | after 2-phase fixed-seed chaos: Σ(class_size-req)/Σreq, page utilization (inuse/objects per page), peak/steady live_pages | steady state doesn't grow, internal fragmentation rate reported | bounded and printed | two-phase delta detects leaks |
| `test_latency_distribution` | latency distribution | single class 128B, 5M alloc/free, record ns each time, output P50/P95/P99/max | distribution table | recording is the deliverable | use rdtsc or clock_gettime(CLOCK_MONOTONIC_RAW); warmup 10k |
| `test_multithread_percpu` | per-CPU path concurrency | **shim upgrade**: `this_cpu()` returns thread-local cpu_t (id=0..N-1), 4 threads each bound to a virtual CPU run chaos + cross-thread free of each other's pointers | 0 duplicates, 0 leaks | pass | thread-local id + fixed seed; ASAN variant |
| `test_size_class_throughput` | per-class throughput | each class (16..1024) standalone 2s×3 runs throughput + latency | per-class table | recorded | batch timing (one clock per 1M ops) |
| `ops_def_and_measurement` | measurement specification | move timing out: 10k warmup first, then N ops with two clocks; op definition written into report | pure ops/s | reproducible | checksum anti-elimination |

**Report changes**:
- Throughput numbers must annotate: op definition (with/without memset, clock calls), warmup, run duration, variance, core pinning or not, WSL2 virtualization noise.
- "Same order of magnitude as same-machine glibc, so not a bottleneck" → **delete or downgrade**: the glibc comparison was **never actually measured**; throughput ≠ bottleneck conclusion. If kept, must actually measure glibc malloc/free under the same load in the same process for three numbers, and the bottleneck conclusion needs kernel-side profile (syscall/IO share) support.
- "This component can be judged stable" → **downgrade**: "Under single-threaded, no-fault-injection, host-shim environment, algorithmic correctness and memory safety within the S1-S10 coverage behave well; SMP safety, OOM tolerance, and fragmentation characteristics are not yet verified."
- live_pages=9: add `dump_live_pages_sites` (record the call site of every VMM::Alloc in the shim), explain the composition of the 9 pages before writing conclusions.

---

## 2. File Cache Gaps and Retest Plans

### 2.1 Missing Evidence Checklist

| # | Gap | Current state |
|---|---|---|
| F1 | Per-path throughput | Only promote; get(hit)/get(miss+migrate)/put/invalidate/idle_handler have no standalone numbers |
| F2 | Mixed load and hit rate | No hit/miss statistics output, no read/write ratio sweep |
| F3 | Key length/ART depth | Keys are only "file0".."file199" (5-7B); no 64/256/1024B keys, no binary keys, no adversarial prefix-family sweep |
| F4 | **Minimal deterministic regression tests** for the three fixed defects | Currently only embedded in the big chaos, no standalone named tests |
| F5 | SMP concurrency | Single-threaded; `g_fc_cpus` multi-cache-instance broadcast/migration concurrency semantics untested (multiple threads on the same cache instance counts as concurrency) |
| F6 | Before/after fix performance comparison | Hot-path cost of fix 2 (TryGetSize per reuse) and fix 1 (per key[depth] branch) not quantified |
| F7 | flush/writeback path content correctness | wb_cb always returns 0, never actually verifies written-back data; is_dirty cleared after flush, CRC update not verified end-to-end |
| F8 | Failure injection | fc_kmalloc_with_fallback's eviction fallback never triggered (kmalloc always succeeds) |
| F9 | Broadcast while pinned | Path where pin>0 entries get broadcast-INVALID has no targeted test |

### 2.2 Minimal Regression Tests for the Three Fixed Defects (P0, assertions directly copyable)

| Test name | Defect | Minimal reproduction | Assertion |
|---|---|---|---|
| `test_art_true_prefix_key` | ART true-prefix key out-of-bounds read (fix 1) | art_mini: fill key buffer tail with junk bytes (`malloc(8); memcpy(buf,"file10",6); buf[6]='X'`) then insert existing "file105"; reverse: insert "file10" first then "file105" | after insert, `art_search("file10")` hits, `art_search("file105")` hits, `art_verify` no violations; **before the fix this test always fails when buf[6] is non-zero** |
| `test_promote_inline_capacity_reuse` | promote inline capacity reuse overflow (fix 2) | promote(data_len=8) then promote(data_len=64) on the same key; allocate an adjacent sentinel entry, verify its canary | sentinel canary intact; second promote returns 0; get returns 64B content consistent |
| `test_insert_fail_no_dangling` | dangling reference/double free after failed insert (fix 3) | construct art_insert then search mismatch (using `test_art_true_prefix_key`'s old trigger or injection): after promote returns -4, `art_iter` whole tree has no residue of that key; promoting the same key again succeeds | after -4, tree has no residual value; no FCDBG double-free/dangling; repeat 100 times no leak |

### 2.3 Other Retests

| Test name | Purpose | Design | Metrics | Anti-self-deception |
|---|---|---|---|---|
| `test_fc_path_throughput` | per-path throughput | prefill 200 keys → 2s×3 separately measure get-hit/get-miss(migrate)/put/invalidate/promote/idle_handler | per-path ops/s + variance | batch timing, warmup, fixed seed |
| `test_fc_hit_rate` | hit rate and mixed load | read/write ratio ∈ {9:1, 1:1, 1:9}, key skew (Zipf), 4 cache instances | hit/miss/migration/eviction counters | compare against shadow-model expectations |
| `test_key_length_sweep` | ART depth | key lengths {5,16,64,256,1024}, prefix-family adversarial (file1/file10/file105/same-length random), 100k ops | 0 tree corruption, search hits correct | art_verify every op |
| `test_fc_concurrent` | SMP concurrency semantics | 4 threads sharing 1 cache instance (true concurrency, shim spinlock actually engages) + 4 threads × 1 instance each broadcasting to each other | 0 crashes 0 leaks | ASAN variant + uniqueness |
| `test_flush_writeback_content` | writeback correctness | wb_cb captures data for CRC comparison; promote(is_dirty) → idle → assert wb_cb receives consistent content, is_dirty cleared | content consistent | callback-side checksum |
| `test_fc_oom_fallback` | eviction fallback | kmalloc failure injection → assert fc_kmalloc_with_fallback triggers eviction and returns a usable block | no crash, recoverable | fault-injection switch |
| `test_broadcast_pinned` | pinned broadcast | get(holding pin) → other core invalidate → assert state INVALID, no dangling after put | no FCDBG warnings | negative assertion |

**Report changes**:
- "All pass" → change to "the 400,000-op chaos, destroy, and three-run promote throughput listed in this report pass" (state the scope).
- promote throughput: add variance, op definition (incl. broadcast and kcopy), warmup.
- "After the fix, 4 cores 300 seconds without #GP" → **downgrade and correct**: "QEMU(TCG) 4-core idle desktop 150-second smoke without anomalies (early 300-second rounds predate the ART/inline fixes); not a proof of SMP correctness, needs F5 concurrency tests + fault injection to strengthen."

---

## 3. Scheduler Gaps and Retest Plans

### 3.1 Mechanism Comparison with Linux EEVDF (corrected via line-by-line source reading)

**Important correction (2026-09-27)**: the earlier model replica and report described sched.cpp
as "CFS leftmost vruntime + RIP heuristic" — **after reading the source this is confirmed to be
an understatement**. The real source's selection skeleton is already EEVDF: deadline-sorted
red-black tree (`thread_rb_cmp` :574-582), eligible subtree-augmented descent search (:897-919),
**min-deadline selection among eligible** (:889-894 fallback), wakeup preemption + remaining
slice protection (:1220-1230), lag clamping (:588-605), weight-scaled quantum + per-thread
`custom_quantum` override (:205-214). Model mode 0 has been rewritten to match the real source,
report §3 qualitative claims corrected.

| Mechanism | Linux EEVDF (6.6+) | sched.cpp real source | Gap |
|---|---|---|---|
| Virtual deadline VD | `ve + slice/weight`, eligible set sorted by VD | **Aligned (2026-09-27)**: `deadline = vruntime + slice` + **deadline protection** (not extended until vruntime passes the old deadline, Linux update_deadline semantics) — this kernel's slice is already weight-scaled, one full slice's vruntime consumption is always base_quantum, so `ve + slice` and `ve + slice/w` are **isomorphic** in their respective units | ✅ aligned |
| eligibility / lag | explicit lag≥0 + lag accounting | **Aligned (2026-09-27)**: `vruntime ≤ avg` is the equivalent form of lag≥0 (weight>0); added **vlag accounting** — sleep dequeue saves `vlag=(avg−vruntime)·weight` (Linux update_entity_lag), wakeup places at `vruntime = avg − vlag/weight` (Linux place_entity), then symmetric clamping [avg−slice, avg+2·slice] | ✅ aligned (clamp thresholds still constants ±slice/±2slice, Linux uses latency-scale thresh — small residual difference) |
| protect_slice | preemption protection | **exists**: wakeup preemption requires current remaining slice ≥ ¼ base_quantum (:1221-1230), otherwise no interruption; + added deadline protection | ✅ exists (simplified version) |
| wakeup preemption | `wakeup_gran` check | **exists**: waker eligible and smaller deadline → interrupt (:1220) | ✅ exists |
| lag clamping | placement compensation | **exists**: vruntime ∈ [avg−slice, avg+2·slice] (:591-602) | ✅ exists (symmetric clamping) |
| slice interface | sched_setattr | **exists**: per-thread `custom_quantum` override (:209-211) | ✅ exists |
| quantum weight scaling | slice ∝ weight | **exists**: `base_quantum × weight / 1024` (:212-213) | ✅ exists |
| multi-queue balance | runqueue push/steal | model is single-queue; real source has TryPush/steal (:765, :859) | model doesn't cover it (real source has it) |

**Verification (2026-09-27 alignment round)**: kernel 0 warnings 0 errors, QEMU smp 2/4 boot
without anomalies; after model mode 0 synchronized replica: fairness shares still precise (max
deviation 0.0056%, deadline protection makes convergence slightly looser but PASS), spawn/exit
chaos 0 failures, wakeup→run latency 144 ticks vs Linux full reference 7 ticks (tradeoff of
remaining-slice protection, a cost explicitly noted in source comments).

**Downgraded statement (kept)**: "Under the ledger model (single queue, fixed weights, faithful
replica of the real selection skeleton), 200k-tick shares match weights, vruntime max deviation
0.0012%; excludes multi-queue balance semantics, model throughput/latency numbers do not
correspond to real kernel performance."

### 3.2 Reinforcement Plans

| Plan | Nature | Content | Deliverable |
|---|---|---|---|
| `sched_ref_eevdf` | formula replica | implement a reference model per Linux EEVDF equations (VD=avg+slice/w, eligible set, protect_slice, lag), compared **side-by-side under the same load** against the 3EVDF replica | two-model fairness/tail-latency comparison table; quantify the cost of "unweighted deadline + no protect_slice" |
| `sched_wake_sleep_model` | formula replica | add random sleep/wake (exponential distribution), measure wake→run latency P99, RIP on/off | real measure of interactive latency (replacing the current "max pick interval") |
| QEMU in-kernel accounting trace (**real code**) | real source + kernel print | extend `kernel/src/arch/x86_64/schedule/syscall/sched_bench.cpp`: every N ticks output each thread's vruntime/deadline/quantum/avg_vruntime to serial, QEMU `-serial file:` capture then offline analysis | real ledger convergence/drift curves under SMP; evidence one level stronger than the model |

---

## 4. Methodology and Reproducibility Gaps

| # | Gap | Retest/fill |
|---|---|---|
| M1 | Environment context | report adds: `uname -a`, CPU model, core count, taskset or not, WSL2 version, QEMU version and accelerator (TCG/KVM) |
| M2 | Anti-optimization | all stress loops: warmup + batch timing + checksum sink print; one `-O0/-O2` comparison |
| M3 | Seeds and golden samples | report lists the three seeds; each test generates an expected-output snapshot file, CI diff detects regressions |
| M4 | CI | `tests/ci.sh`: `make slub fc sched && ./bin/*_test` all exit 0 to pass; attach GitHub Actions linux runner yml |
| M5 | Load declaration | every number annotated: op definition, warmup, duration, run count, variance, clock calls or not |
| M6 | Negative test checklist | S7/F8/test_oom_path/test_fc_oom_fallback/test_insert_fail_no_dangling included in CI |

---

## 5. Five Statements Judged One by One

| Statement | Judgment | Handling |
|---|---|---|
| "This component can be judged stable" | **overstated** | downgrade to "under single-threaded shim, no fault injection environment, algorithmic correctness within covered scope behaves well; SMP/OOM/fragmentation not verified" |
| "All pass" | **vague** | only use after listing the test scope |
| "Same order of magnitude as same-machine glibc malloc/free, so not a system bottleneck" | **unsupported** | glibc not actually measured; bottleneck conclusion needs kernel profile; delete or rewrite after measurement |
| "Fairness strictly verified" | **overstated** | restrict to "ledger model, single queue, no sleep/wakeup" share consistency |
| "After the fix, 4 cores 300 seconds without #GP" | **inaccurate** | correct to 150-second TCG smoke (300-second rounds predate ART/inline fixes); positioned as smoke, not proof |

---

## 6. Truthfulness Matrix Template

| Module | Source file | Real source compiled | shim/replica/simulation | Verifiable boundary | Next verification approach |
|---|---|---|---|---|---|
| SLUB/SLAB | `kernel/src/mem/heap.cpp` | ✅ real source (`-D__KERNEL_TEST_HOST__` only disables cli, adds ASAN hooks) | shim: VMM page pool, this_cpu, spinlock (no contention) | algorithmic correctness, single-threaded memory safety | multithreaded per-CPU shim, OOM injection, QEMU long stability |
| File cache | `kernel/src/fs/fc.cpp` + `art.c` + `heap.cpp` | ✅ real source | shim same as above; `g_fc_cpus` multi-instance single-threaded | single-core multi-cache-instance semantics | true concurrent multithreading, QEMU disk load |
| ART | `art.c` | ✅ real source | none (art_mini compiled directly) | full semantics | already includes prefix/chain/delete; add deep keys + concurrency |
| Scheduler | `tests/sched/main.cpp` | ❌ | **formula-replica model** (constants/formulas annotated against sched.cpp line numbers) + multimap substitute | ledger math | QEMU sched_bench trace (real code) or sched_core.h extraction |
| Whole kernel | ISO | ✅ | none | QEMU TCG smoke | KVM/real machine, fault injection, stress load |

---

## 7. Minimal Executable Retest Roadmap

| Priority | Item | Estimated effort | Dependency | Deliverable |
|---|---|---|---|---|
| **P0** | `test_art_true_prefix_key` + `test_promote_inline_capacity_reuse` + `test_insert_fail_no_dangling` (three minimal regressions) | 0.5 day | none | three standalone runnable tests + golden samples |
| **P0** | report corrections: downgrade five statements / complete context (op definition, environment, variance, seeds, 150s smoke) | 0.25 day | none | revised stability-report.md |
| **P0** | `test_boundary_sizes`, `test_krealloc_shrink_grow`, `test_kcalloc_overflow`, `test_oom_path`, `fragmentation_steady_state` | 0.5-1 day | shim adds fault-injection switch | SLUB boundary+OOM+fragmentation evidence |
| **P1** | `test_size_class_throughput` + `test_latency_distribution` (per-class + latency, batch timing + warmup) | 0.5 day | measurement specification | per-class throughput/latency table (with context) |
| **P1** | `test_fc_path_throughput` + `test_fc_hit_rate` + `test_key_length_sweep` + `test_flush_writeback_content` + `test_broadcast_pinned` | 1 day | none | fc per-path/hit-rate/deep-key evidence |
| **P1** | `sched_ref_eevdf` + `sched_wake_sleep_model` | 1 day | none | two-model comparison + tail-latency data |
| **P1** | live_pages composition dump (`dump_live_pages_sites`) | 0.25 day | none | 9-page mechanism explanation |
| **P2** | `test_multithread_percpu` + `test_fc_concurrent` (shim multi-CPU + true concurrency) | 1-2 days | shim upgrade (thread-local this_cpu, real spinlock stress) | host-level SMP semantics evidence |
| **P2** | kernel `sched_bench` extension + QEMU serial trace capture and analysis | 1-2 days | kernel rebuild process | real-code SMP ledger curves |
| **P2** | CI: `tests/ci.sh` + GitHub Actions + golden-sample diff | 0.5 day | golden samples from P0/P1 tests | regression gate |
| **P2** | before/after fix performance comparison (art guard branch, TryGetSize reuse-check overhead) | 0.5 day | switchable comparison build | overhead quantification (expected <1-3%, optimize if >5%) |

**Suggested execution order**: all P0 → all P1 → P2 as needed (P2's multithreading and QEMU trace have the highest value; CI and performance comparison can come later).
