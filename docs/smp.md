# SMP: Multi-Core Boot and per-CPU

Source: `kernel/src/arch/x86_64/smp/smp.cpp` (about 9 KB).

## 1. Boot Sequence

CPUs are enumerated via the **Limine multiprocessor protocol**, and the BSP boots the APs:

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

Each AP lands in `smp_cpu_init(mp_info)`:

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
  → LAPIC::Oneshot(SCHED_VEC, base_quantum*ticks)   // key: arm the first tick
  → sti
  → sched_idle()   ← does not return; the first Switch snapshots this context as the idle thread
```

## 2. How the per-CPU Pointer Is Obtained

- After `smp_started`, `this_cpu()` is simply `movq %%gs:0, %0` — the GS base was written to `IA32_GS_MSR` at AP startup, so each core reads its own `cpu_t*` with no lock and no array index.
- Before `smp_started` (very early boot), it always returns `bsp_cpu`.
- per-CPU data: `dyn_ctx[]` (scheduling feedback statistics), `per_cpu_steal_throttle[]`, `per_cpu_steal_cursor[]`, `g_tlb_batch[]`, `cslab` (SLAB free chain), `file_cache`.

## 3. Hardware Capability Detection and Fallback

| Feature | Detection | When Unsupported |
|---|---|---|
| FSGSBASE | `cpuid.(7,0).ebx & (1<<5)` | `WRFSBASE` falls back to the plain version, no crash |
| SMEP/SMAP | `enable_smep_smap()` | — |
| XSave / AVX | `simd_cpu_init`, stores `XsaveSize/MaskLo/MaskHi` | the thread fx_area is allocated at its actual size |
| SSE/FPU | `fpu_init()` | unsupported → `hcf()` immediately |

## 4. Two Key Boot-Time Deadlock/Stall Fixes

Recorded in the source comments:

1. **The AP must arm its first periodic tick before going idle.** If the AP executes `hlt` while its LAPIC timer is off and a remote wake-up IPI happens to get lost, it will sleep until the next unrelated IPI — an observed stall of about 14 seconds. Arm the oneshot first and only then `hlt`, so every base slice gets pulled back by the scheduler at least once.
2. **IPI wake-up**: after pushing a task onto a core that is currently in `hlt`, immediately issue `LAPIC::IPI(target->lapic_id, SCHED_VEC)` instead of waiting for it to wake up on its own.

## 5. Interrupts and Timer Buckets

- Each CPU has a three-level timer wheel `tv1/tv2/tv3` (the typical kernel timer tiering); the `timer_cpu` field records which core owns a thread's timer. When a thread is stolen/pushed, `timer_cpu` is updated in sync so the timer never expires on the old core.
- SCHED_VEC is the scheduler IPI vector; `LAPIC::IPI` / `IPIOthers` are used for cross-core scheduling.
- The IOAPIC routes GSIs to a designated LAPIC; PIT GSI0 is explicitly redirected to vector 32 on the BSP (see architecture.md).

## 6. Affinity

- `cpu_simd_mask(cpu)` returns the core's **SIMD instruction-set bitmap** — note it is not an SMT/topology mask (the kernel holds no APIC-ID/core-sibling topology data at all), so the old docs' "SMT" claim is inaccurate;
- load balancing (see scheduler.md) prefers to push/steal within the same mask;
- the GUI compositor's workers are pinned by the online CPU count, one horizontal strip per core (see gui.md).

## 7. The Wall Clock Can Freeze — Never Use `PIT::TimeSinceBootMS()` As A Deadline

**Invariant:** PIT GSI0 is routed to the BSP only (`IOAPIC::RemapIRQ(bsp_cpu->lapic_id, 0, 32)`
in `init.cpp`), so `TicksSinceBoot` is incremented **only** in the BSP's interrupt
context. Any CPU that spins on a spinlock with interrupts disabled — and the BSP in
particular — therefore freezes the *global* wall clock. Every wait bounded by
`PIT::TimeSinceBootMS()` immediately degenerates into an unbounded spin.

This turned ordinary lock contention into a full-system hang:

```
BSP spins on a lock with IF=0
  -> PIT IRQ0 not delivered -> TicksSinceBoot frozen
  -> a lock holder inside LazyTLB::WaitAcks never reaches its 100 ms deadline
  -> it never releases pt_lock/vma_lock
  -> the BSP spins forever
```

`VMM::Free` makes this easy to hit: it takes `pm->vma_lock` + `pm->pt_lock`, then
`LazyTLB::BatchBegin()` issues `cli`, and `FreeOwnedRegion` calls
`LazyTLB::ShootdownFence` from inside that window — i.e. it waits for remote ACKs
with interrupts disabled, using the clock it is itself freezing.

**Rules:**

1. Any deadline/timeout must use **`PIT::MonotonicMS()`** (TSC-derived once
   `sched_tsc_per_ms()` is calibrated, PIT otherwise). Keep `TimeSinceBootMS()` for
   accounting/uptime only.
2. Always pair a wall-clock deadline with a **bounded spin count**, so a completely
   broken clock still cannot wedge a core.
3. Do **not** wrap a remote lock acquisition in `cli/sti` "just in case".
   `LazyTLB::SendTarget` used to do this around the *target's* queue lock; on the BSP
   it blocked the PIT IRQ, and because the local IPI handler only ever takes *this*
   CPU's queue lock (and we never send to ourselves) the `cli` bought nothing.
4. When collapsing/short-cutting a queue, **settle every dropped request's ACK**
   (`DoAck`). Discarding work without settling the ACK leaves the initiator waiting on
   a completion that can never arrive.
5. `spinlock_unlock` must be a **release** store. A relaxed `movl` lets the compiler
   sink critical-section writes past the unlock.

### 7.1 Follow-up: make the clock itself stall-proof

Converting every deadline to `MonotonicMS()` is not enough, because the timer wheel,
`Schedule::Sleep` and vruntime accounting all read `TimeSinceBootMS()`; changing only
the waiters would leave two disagreeing clocks. So `TimeSinceBootMS()` now carries its
own guard: when the PIT falls more than `PIT_STALL_GUARD_MS` (250 ms) behind the TSC it
returns the TSC value instead, and a CAS-raised monotonic floor (`g_ms_floor`) keeps
`now` from going backwards when the PIT catches up again.

The 250 ms threshold is deliberately far larger than any plausible TSC-vs-PIT drift, so
on a healthy machine the function is bit-for-bit what it was before; it only engages on
a genuine freeze.

### 7.2 A wedged `printk` silences the whole machine

`printf_` formats into a stack buffer, then emits it byte by byte **while holding
`ptf_lock`**. Any unbounded wait on that path is therefore not a local problem: every
other core that tries to log blocks on the same lock, so the serial log stops dead —
often mid-line (`[INFO] [lw`). When the log goes quiet, do not assume the last printed
line is where it hung.

`Serial::_Write` used to be `while (!_CanWrite());`. It is now bounded.

### 7.3 Scheduling-side waits

`wait_for_transfer`, `kill_thread_batch` and `WaitForThreadOffCpu` (all in `task.cpp`)
run in the process-exit path. Their "give up instead of Panic" escapes were all computed
from `TimeSinceBootMS()`; when that clock froze the escapes never fired and the loops
became **silent** infinite spins. They now use `MonotonicMS()` plus a spin-count floor.
`WaitForThreadOffCpu` still keeps waiting rather than returning early — freeing a thread
that is still current on another core is a use-after-free.

### 7.4 Calibrate the TSC against HPET, not the PIT

`sched_calibrate_tsc()` used to measure the TSC against `PIT::TimeSinceBootMS()`, which
only advances inside the BSP's IRQ0 handler. The calibration runs on the boot path right
after `Schedule::Install()`, where the BSP is typically inside an interrupts-off window —
so the PIT window measured 0 ms and `g_tsc_per_ms` stayed 0 forever. That silently
disabled every `MonotonicMS()` deadline (TLB shootdown ACKs, process-exit waits) and the
`TimeSinceBootMS()` stall guard; the system then only survived on spin-count floors.

The baseline is now `HPET::GetTimeNS()` — an MMIO read of the main counter, with no
interrupt dependency. Two traps worth remembering:

- `HPET::GetTimeNS()` falls back to returning `PIT::TicksSinceBoot` when HPET is absent.
  That is **ticks, not nanoseconds**. Any caller using it as an ns baseline must check
  `HPET::Available()` first.
- The result is sanity-checked (100 MHz – 100 GHz). A mis-parsed `counter_clk_period`
  would otherwise poison every deadline at once.

### 7.5 Detect a frozen PIT by stall, not by drift

The `TimeSinceBootMS()` guard triggers when the **PIT reading itself has not changed**
while the TSC advanced past `PIT_STALL_GUARD_MS` — not when "the TSC leads the PIT".
The distinction matters: the drift-based test is not immune to TSC calibration error (a
TSC that runs 2x fast would trip it permanently and quietly change scheduler behaviour),
whereas the stall-based test can only fire when the tick genuinely stops. On a healthy
machine it never fires and the function is bit-for-bit what it was.
