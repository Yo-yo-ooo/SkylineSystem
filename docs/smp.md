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
