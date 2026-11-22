<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# Memory subsystem

Source: `kernel/src/arch/x86_64/vmm/` (`vmm.cpp`, `vma.cpp`, `ua.cpp`), `kernel/src/mem/` (`pmm.cpp`,
`heap.cpp`, `new.cpp`).

## 1. Layering

```text
User-mode mmap / munmap  ← syscall 14/15
        ↓
VMA management (vma.cpp)         region abstraction, page-fault handling, extend/split
        ↓
VMM page tables (vmm.cpp)        4-level page tables (PML4→PDPT→PD→PT), huge-page mapping
        ↓
PMM (pmm.cpp)              physical page-frame allocation (sharded bitmap locks + per-CPU cache)
        ↓
SLUB / SLAB (mem/heap)     small-object kernel heap kmalloc(16..1024B)
```

## 2. VMM: 4-level paging and huge pages

- `CONFIG_VMM_5LVL_MAP = 0`, so the running config is 4-level paging; the 5-level path is compiled
  out. One bug worth knowing: `IsPM5LVL` once had its sense inverted, which made the user address
  upper bound use 5-level constants. Both now use the 4-level bound.
- 1 GB and 2 MB huge pages can be allocated and mapped.
- Copy-on-Write: fork and shared mappings mark the PTE read-only with the COW bit; the first write
  faults and gets a fresh page. The granularity is the catch: a 2 MB huge page is copied whole when
  it faults, not split into 4 KB pages. Only 1 GB pages split, down to 2 MB. There is no
  split-then-remerge; one of the older documents claimed there was.
- A process's address space is separate from `kernel_pagemap`; each process PML4 copies the kernel
  high-half entries.
- `VMM::Alloc/EAlloc/Free` are the kernel-side allocation primitives. `sys_munmap` already frees a
  VMA range partially, by length.

## 3. TLB shootdown

- Per-CPU batch contexts in `g_tlb_batch[MAX_CPU]`. Invalidations inside one critical section
  accumulate; on commit each target CPU takes a single IPI.
- If the batch overflows it degrades to one full flush, not a per-page fallback. An earlier doc
  described it the other way.
- `DestroyPM` waits for each remote shootdown queue to drain before releasing the pagemap. That wait
  is the lite version: a spin cap plus logging. Per-CPU completion counting isn't implemented, so the
  window is narrowed rather than closed.
- Under the PCID path `cpus_with_tlb` only ever grows, so an invalidation is broadcast to every core
  that has ever run this address space. That is the gap against the anti-IPI-storm ideal.

## 4. PMM: physical pages

- The bitmap is sharded: each shard owns a 2 GiB window (`SHARD_PAGES`) with its own L1/L2/L3
  summary words and its own lock, and the allocator walks shards round-robin. The old
  "serialised by one global `pmm_lock`" description is stale: `pmm_lock` is only taken by the
  slow path (capability/slow operations) and no longer serialises normal allocations. The
  "lock-free bitmap with CAS" claim in an even older doc was also wrong; there is no CAS bitmap.
- Each CPU keeps a small-page cache. Single-page requests that hit the cache never take a shard
  lock. Huge pages (2MB/1GB) scan their shard under the shard locks.
- The free path has basic protection for the per-CPU cache, and kernel/reserved regions are excluded
  correctly at init.

## 5. SLUB / SLAB heap

Boot order is strict: `PMM::Init() → VMM::Init() → SLAB::Init() → SLUB::InitKmalloc() →
SLUB::SelfTest()`. A self-test failure prints `LastFailStage()` and falls back to SLAB.

- Size classes 16..1024B, per-CPU magazines, in-page free lists with an XOR cookie, a CAS Treiber
  free stack.
- `krealloc/kcalloc` check for overflow. SLUB's `Free` detects double frees (`inuse==0`) and rejects
  them through `slab_fatal`; the SLAB side had the same guard first.
- QSBR isn't here. Deferred reclamation belongs to the user allocator
  `lib/stdc/allocator/allocator.c`; the kernel heap has no grace-period reclaim. An older doc
  attributed it to the wrong subsystem.

## 6. Hot primitives

`ablib/arch/x86_64/x86mem/` provides multi-version `memcpy/memset/memmove/memcmp` (base, AVX, AVX2,
picked by CPUID at boot). It comes from the third-party x86mem library; attribution is in the file
headers.

## 7. VMA, page faults, user copies

- `vma.cpp` tracks `[start, end)`, permissions and a red-black tree index. Page faults dispatch to
  anonymous, file or CoW handling.
- `ua.cpp` (`CopyToUser/CopyFromUser`) enforces the `MM_USER` check and the 4-level user-half upper
  bound. Before that, user mode could read and write arbitrary physical memory through the
  `0x800000000000+p` alias. Fixed.
- When a CoW page faults on write, the COW bit decides: writing to a mapping that was forked
  read-only allocates a new page.

## 8. Security state

- NX/W^X: EFER.NXE is set where CPUID allows. After the ELF load finishes, segments are tightened by
  `p_flags` (non-writable loses W, non-executable gains NX). With NXE disabled the NX bit used to be
  reserved and setting it raised #PF.
- KASLR is the bootloader's alone (`-fPIE/-pie`), no kernel-side logic. After a build,
  `make kaslr-check` confirms no 32-bit absolute relocations remain.
- SMEP and SMAP are on. Kernel access to user buffers goes through the `ua.cpp` wrapper.
