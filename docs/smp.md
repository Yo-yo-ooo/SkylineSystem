# SMP: multi-core boot and per-CPU state

Source: `kernel/modules/builtin/x86_64/smp/smp.cpp` (about 9 KB).

## 1. Boot

CPUs are enumerated through the Limine multiprocessor protocol. The BSP brings the APs up.

```text
smp_init()  (on the BSP)
  ├─ record bsp_cpu->lapic_id, build the apic_id_to_logical[] mapping
  ├─ smp_setup_thread_queue(bsp)   // one RBTree runqueue per CPU
  ├─ smp_setup_kstack(bsp)         // kernel stack + TSS IST/RSP0
  ├─ EnableFSGSBASE(bsp)
  ├─ simd_cpu_init(bsp)
  └─ iterate over mp_response->cpus[]:
        allocate a cpu_t for each AP, fill in lapic_id/logical_id,
        set mp_info->goto_address = smp_cpu_init,
        the BSP waits in a pause loop until started_count == the number of remaining cores
```

Each AP enters `smp_cpu_init(mp_info)`:

```text
switch to kernel_pagemap
  → GDT::Init(logical_id) + idt_reinit(logical_id)
  → take smp_lock, fill in cpu_t: self/id/lapic_id, initialize the cslab free list
  → wrmsr KERNEL_GS_BASE / IA32_GS_MSR = cpu_t*   ← the foundation of this_cpu()
  → LAPIC::Init + InitTimer
  → EnableFSGSBASE(cpu) (if unsupported, WRFSBASE falls back to the plain version)
  → smp_setup_kstack
  → idt_install_irq_cpu(SCHED_VEC, Schedule::Internal::Switch)
  → syscall_init()
  → smp_setup_thread_queue()
  → sse_enable / fpu_init / simd_cpu_init / cpu_simd_mask
  → enable_smep_smap()
  → zero the tv1/tv2/tv3 timer buckets
  → allocate the per-CPU file_cache and register the write-back callback
  → started_count++, update smp_last_cpu
  → LAPIC::Oneshot(SCHED_VEC, base_quantum*ticks)   // arm the first tick
  → sti
  → sched_idle()   ← does not return; the first Switch snapshots this context as the idle thread
```

## 2. Getting the per-CPU pointer

- Once `smp_started` is set, `this_cpu()` is `movq %%gs:0, %0`. The GS base was written to
  `IA32_GS_MSR` at AP startup, so every core reads its own `cpu_t*` without a lock or an array index.
- Before `smp_started`, during very early boot, it returns `bsp_cpu`.
- Per-CPU data: `dyn_ctx[]` (scheduling feedback stats), `per_cpu_steal_throttle[]`,
  `per_cpu_steal_cursor[]`, `g_tlb_batch[]`, `cslab` (SLAB free chain), `file_cache`.

## 3. Feature detection and fallbacks

| Feature | Detection | When unsupported |
|---|---|---|
| FSGSBASE | `cpuid.(7,0).ebx & (1<<5)` | `WRFSBASE` uses the plain version, no crash |
| SMEP/SMAP | `enable_smep_smap()` | — |
| XSave / AVX | `simd_cpu_init` stores `XsaveSize/MaskLo/MaskHi` | the thread fx_area is sized to what the CPU actually offers |
| SSE/FPU | `fpu_init()` | `hcf()` immediately |

## 4. Two boot-time stalls and their fixes

1. An AP must arm its first tick before idling. An AP that `hlt`s with its LAPIC timer off can
   sleep until some unrelated IPI arrives if the wake-up IPI is lost — 14 seconds was observed. Arm
   the oneshot first, then `hlt`, so the scheduler pulls every base slice back at least once.
2. IPI wake-up on push. After pushing a task onto a core sitting in `hlt`, issue
   `LAPIC::IPI(target->lapic_id, SCHED_VEC)` right away instead of waiting for it to wake on its own.

## 5. Interrupts and timer buckets

- Each CPU owns a three-level timer wheel `tv1/tv2/tv3`. The `timer_cpu` field records which core
  owns a thread's timer, and it is updated whenever a thread is stolen or pushed, so a timer never
  expires on the old core.
- SCHED_VEC is the scheduler IPI vector; `LAPIC::IPI` and `IPIOthers` drive cross-core scheduling.
- The IOAPIC routes GSIs to a chosen LAPIC. PIT GSI0 is redirected to vector 32 on the BSP, see
  [architecture.md](architecture.md).

## 6. Affinity

- `cpu_simd_mask(cpu)` is the core's SIMD instruction-set bitmap, not an SMT or topology mask. The
  kernel holds no APIC-ID or core-sibling topology data at all.
- Load balancing prefers pushing and stealing within the same mask; see [scheduler.md](scheduler.md).
- Compositor workers are pinned by online CPU count, one horizontal strip per core; see
  [gui.md](gui.md).

## 7. The wall clock can freeze, so never use `PIT::TimeSinceBootMS()` as a deadline

PIT GSI0 goes to the BSP only (`IOAPIC::RemapIRQ(bsp_cpu->lapic_id, 0, 32)` in `init.cpp`), which
means `TicksSinceBoot` only increments in the BSP's interrupt context. Any CPU spinning on a lock
with interrupts disabled — the BSP especially — freezes the global wall clock, and every wait
bounded by `PIT::TimeSinceBootMS()` turns into an unbounded spin.

That is how ordinary lock contention became a whole-system hang:

```
BSP spins on a lock with IF=0
  -> PIT IRQ0 not delivered -> TicksSinceBoot frozen
  -> a lock holder inside LazyTLB::WaitAcks never reaches its 100 ms deadline
  -> it never releases pt_lock/vma_lock
  -> the BSP spins forever
```

`VMM::Free` walks straight into this: it takes `pm->vma_lock` and `pm->pt_lock`,
`LazyTLB::BatchBegin()` issues `cli`, and then `FreeOwnedRegion` calls `LazyTLB::ShootdownFence`
from inside that window — waiting for remote ACKs with interrupts off, on a clock it is itself
freezing.

Rules that came out of it:

1. Deadlines use `PIT::MonotonicMS()` (TSC-derived once `sched_tsc_per_ms()` is calibrated, PIT
   otherwise). `TimeSinceBootMS()` is for accounting and uptime only.
2. Pair every wall-clock deadline with a bounded spin count, so even a completely broken clock can't
   wedge a core.
3. Don't wrap a remote lock acquisition in `cli/sti` "just in case". `LazyTLB::SendTarget` used to do
   that around the target's queue lock; on the BSP it blocked PIT IRQs, and since the local IPI
   handler only ever takes this CPU's queue lock and we never send to ourselves, the `cli` bought
   nothing.
4. When a queue is collapsed or short-circuited, settle every dropped request's ACK (`DoAck`).
   Discarding work without settling leaves the initiator waiting for a completion that can't arrive.
5. `spinlock_unlock` must be a release store. A relaxed `movl` lets the compiler sink
   critical-section writes past the unlock.

### 7.1 Making the clock stall-proof

Switching every waiter to `MonotonicMS()` isn't enough: the timer wheel, `Schedule::Sleep` and
vruntime accounting all read `TimeSinceBootMS()`, and two disagreeing clocks are worse than one bad
one. So `TimeSinceBootMS()` got its own guard. When the PIT falls more than
`PIT_STALL_GUARD_MS` (250 ms) behind the TSC it returns the TSC value, and a CAS-raised monotonic
floor (`g_ms_floor`) keeps `now` from going backwards when the PIT catches up.

250 ms is far larger than any plausible TSC-vs-PIT drift, so on a healthy machine the function is
bit-for-bit what it was; it only engages on a genuine freeze.

### 7.2 A wedged `printk` silences the machine

`printf_` formats into a stack buffer and then emits it byte by byte while holding `ptf_lock`. An
unbounded wait there is not local: every other core trying to log blocks on the same lock, and the
serial log stops dead, often mid-line (`[INFO] [lw`). When the log goes quiet, the last printed
line is not where it hung.

`Serial::_Write` used to be `while (!_CanWrite());`. It is bounded now.

### 7.3 Waits on the scheduling side

`wait_for_transfer`, `kill_thread_batch` and `WaitForThreadOffCpu` in `task.cpp` run in the
process-exit path. Their "give up instead of Panic" escapes were computed from
`TimeSinceBootMS()`, so when that clock froze the escapes never fired and the loops became silent
infinite spins. They use `MonotonicMS()` plus a spin-count floor now. `WaitForThreadOffCpu` still
refuses to return early: freeing a thread that is still current on another core is a
use-after-free.

### 7.4 Calibrate the TSC against HPET, not the PIT

`sched_calibrate_tsc()` used to measure against `PIT::TimeSinceBootMS()`, which only moves inside
the BSP's IRQ0 handler. Calibration runs on the boot path right after `Schedule::Install()`, where
the BSP is typically inside an interrupts-off window — so the window measured 0 ms and
`g_tsc_per_ms` stayed 0 forever. That silently disabled every `MonotonicMS()` deadline (TLB shootdown
ACKs, process-exit waits) and the stall guard, and the system survived on spin-count floors alone.

The baseline is `HPET::GetTimeNS()` now: an MMIO read of the main counter with no interrupt
dependency. Two traps:

- `HPET::GetTimeNS()` falls back to `PIT::TicksSinceBoot` when HPET is absent, and that is ticks,
  not nanoseconds. Check `HPET::Available()` before using it as an ns baseline.
- The result is sanity-checked at 100 MHz – 100 GHz; a mis-parsed `counter_clk_period` would poison
  every deadline at once.

### 7.5 Detect a frozen PIT by stall, not by drift

The `TimeSinceBootMS()` guard fires when the PIT reading itself hasn't changed while the TSC has
moved past `PIT_STALL_GUARD_MS` — not when the TSC merely leads the PIT. The difference matters: a
drift test isn't immune to calibration error, since a TSC running 2x fast would trip it permanently
and quietly change scheduler behaviour, while a stall test can only fire when the tick genuinely
stops. On a healthy machine it never fires, and the function stays bit-for-bit identical.
