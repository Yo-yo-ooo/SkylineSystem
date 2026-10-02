# Memory subsystem

Source: `kernel/src/arch/x86_64/vmm/` (`vmm.cpp`, `vma.cpp`, `ua.cpp`), `kernel/src/mem/` (`pmm.cpp`, `heap.cpp`, `new.cpp`).

> This document only describes the code's actual state, including places inconsistent with the old promotional text.

## 1. Layering

```text
User-mode mmap / munmap  ← syscall 14/15
        ↓
VMA management (vma.cpp)         region abstraction, page-fault handling, extend/split
        ↓
VMM page tables (vmm.cpp)        4-level page tables (PML4→PDPT→PD→PT), huge-page mapping
        ↓
PMM (pmm.cpp)              physical page-frame allocation (global lock + per-CPU cache)
        ↓
SLUB / SLAB (mem/heap)     small-object kernel heap kmalloc(16..1024B)
```

## 2. VMM: 4-level page tables and huge pages

- Build config `CONFIG_VMM_5LVL_MAP = 0`, actually runs **4-level paging**; the 5-level path is excluded by compile conditions (earlier a reversed `IsPM5LVL` semantic made the user-mode address upper bound wrongly use 5-level constants, fixed and unified to the 4-level bound);
- Supports **1 GB / 2 MB huge-page** allocation and mapping;
- **Copy-on-Write**: on fork/shared mapping, page-table entries are marked read-only + COW bit; a write faults and allocates a new page. **Granularity limit**: when a 2 MB huge page triggers CoW the whole page is copied (not split into 4K); only 1 GB pages are split into 2 MB on fault; there is **no "split then re-merge"** (the old doc's claim was false);
- User-mode address space isolated from the kernel `kernel_pagemap` (each process's PML4 copies the kernel high-half entries);
- `VMM::Alloc/EAlloc/Free` are the kernel-side allocation primitives; `sys_munmap` already does VMA partial split-free by length.

## 3. TLB shootdown: batch merging

- per-CPU batch context (`g_tlb_batch[MAX_CPU]`): multiple invalidations inside a critical section accumulate into a batch first; on commit each target CPU receives only one IPI;
- On batch overflow it degrades to a **single full flush** (not a per-page fallback —— the old doc's wording was inaccurate);
- `DestroyPM` waits for each remote shootdown queue to drain before releasing the pagemap (**lite version**: spin cap + logging; per-CPU completion counting still unimplemented, the theoretical window is only narrowed, not closed);
- Under the PCID path `cpus_with_tlb` only grows, so cross-core invalidation broadcasts to all cores that historically ran that address space (a gap versus the "anti-IPI storm" ideal).

## 4. PMM: physical page management

- The bitmap frame allocator is serialized by a **single global `pmm_lock`** (the old claim of "lock-free bitmap + CAS" is false; there is no CAS bitmap in the code);
- Each CPU maintains a small-page cache: **single-page requests** hitting locally don't touch the global lock; huge-page (2MB/1GB) requests linear-scan from low addresses under the global lock;
- The free path already has basic protection for the per-CPU cache; kernel/reserved regions are correctly excluded at init.

## 5. SLUB / SLAB kernel heap

Boot order strictly depends on: `PMM::Init() → VMM::Init() → SLAB::Init() → SLUB::InitKmalloc() → SLUB::SelfTest()`; on self-test failure it prints `LastFailStage()` and falls back to SLAB.

- Real slab family: 16..1024B size classes, per-CPU magazines, in-page free lists (XOR cookie), CAS Treiber free stack;
- `krealloc/kcalloc` have overflow checks; SLUB `Free` detects **double free** (`inuse==0`) and rejects with `slab_fatal` (the SLAB side had the same protection originally);
- **QSBR is not here**: deferred reclamation exists only in the **user-mode allocator** `lib/stdc/allocator/allocator.c`; the kernel heap has no grace-period reclamation (the old doc attributed it wrongly, corrected).

## 6. Hot primitive acceleration

`ablib/arch/x86_64/x86mem/` provides multi-version `memcpy/memset/memmove/memcmp` (base/AVX/AVX2, selected by CPUID at boot). This directory originates from the third-party x86mem library; license attribution is in the file header.

## 7. VMA, page faults, and user/kernel copies

- `vma.cpp` records `[start, end)`, permissions, and a red-black tree index; page faults dispatch by anonymous/file/CoW;
- `ua.cpp` (`CopyToUser/CopyFromUser`) enforces the `MM_USER` check and adopts the 4-level user-half upper bound —— previously there was a "user mode reads/writes arbitrary physical memory via the `0x800000000000+p` alias" hole, fixed;
- When granting write permission to a CoW page on fault, it decides by the COW bit (writing after forking a read-only mapping creates a new page).

## 8. Security status

- **NX/W^X**: EFER.NXE enabled per CPUID; after ELF load completes, tighten per segment `p_flags` (non-writable strips W, non-executable adds NX). Previously when NXE was disabled the NX bit was reserved and setting it caused #PF;
- **KASLR**: load randomization is only provided by the bootloader's `-fPIE/-pie`; the kernel itself has no randomization logic; post-build `make kaslr-check` verifies no 32-bit absolute relocations remain;
- SMEP/SMAP enabled; kernel access to user buffers goes through the unified `ua.cpp` wrapper.
