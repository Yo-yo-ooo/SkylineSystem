# SkylineSystem documentation

The root [README.md](../README.md) covers what the system is, how to build it and how to run it. This
directory goes one level down: how the internals are put together, and why they were done that way.

Every claim here was checked against the sources under `kernel/`, not written as copy. Where a comment
says `v3 FIX`, that area really did break once and the fix is the third attempt.

## Documentation map

| Document | Contents | Code it maps to |
|---|---|---|
| [architecture.md](./architecture.md) | Layering, directory layout, boot sequence from Limine to the first ELF | `kernel/src/arch/x86_64/init.cpp` |
| [kernel-pagemap-lock-stall.md](./kernel-pagemap-lock-stall.md) | **Regression fix**: CPU 0 stuck in sys_load on kernel_pagemap->vma_lock; address decoding, IRQ/preemption root cause, KpmIrqGuard fix and churn/smoke/DOSVM verification | `kernel/src/arch/x86_64/vmm/vmm.cpp` |
| [module-implementation-migration.md](./module-implementation-migration.md) | **Implementation files → .ko**: MCR manifests, cross-.ko symbol table, provider-first staging, 4K alignment, moved drivers/input matrix, verification | `kernel/modules/mcr/`, `kernel/Makefile`, `governance.cpp` |
| [kernel-dynlink.md](./kernel-dynlink.md) | **Unified DynModule + modules.saf**: Tier 1/Tier 2 merged, 40 component .ko + 18 demo .ko in one SAF, DLL-style inject/eject/resolve (`kernel.dynlink.v1`), trusted component path, `kernel.component.v1` exports, dosvm .ko, compatibility and verification | `kernel/include/kmod/kdll.h`, `kernel/src/arch/x86_64/kmod/kdll.cpp`, `saf.cpp`, `kernel/Makefile`（构建期生成 `.kout` 导出表） |
| [kernel-modules.md](./kernel-modules.md) | Tier 0 private-contract module ABI (`kmod`): `.in`/`.out`/manifest, ET_REL loader, topology graph, capability governance, transactional load and rollback, hot-replace (quiesce/epoch/watchdog), syscall boundary, seats, Tier 0 shared services (klib/sync/algo direct symbols plus versioned ops contracts) | `kernel/src/arch/x86_64/kmod/`, `kernel/include/kmod/` |
| [kernel-components.md](./kernel-components.md) | Componentisation ledger: Tier 0 + component .ko model (`KCOMPONENT_*` -> .kmod_manifest/.in/.out), modules.saf loading, boot-by-topology mapping, component and contract inventory, migration status, remaining roadmap | `kernel/modules/x86_64/components/`, `kernel/include/kmod/kmod_component.h` |
| [kernel-build.md](./kernel-build.md) | Build pipeline: Tier 0 + shared implementation library, 40 component .ko (`KMOD_COMPONENT_AS_KO`), 18 demo .ko, modules.saf packaging, `kernel.component.v1` exports, ISO layout, adding a component, verification gates | `Makefile`, `kernel/Makefile`, `res/scripts/make_modules_saf.py`, `kernel/include/kmod/` |
| [lua-desktop.md](./lua-desktop.md) | Desktop-embedded Lua 5.4: the `wallpaper` framebuffer variable (addr/width/height/pitch plus draw methods) and `sky.set_window_style`, vendored Lua and freestanding libc additions, QEMU screenshot/serial gate | `programs/desktop/lua/`, `programs/desktop/lua_glue.cpp`, `programs/desktop/desktop.lua`, `lib/stdc/`, `tests/stdc/` |
| [desktop-features.md](./desktop-features.md) | Setup installing the whole OS (SKYOS1/LZ4 package, mkdir syscall 22, `/mp/system` manifest), Settings and Lua themes, Notepad dialogs, WM z-order focus and minimise-keeps-maximise, child exit events (syscall 31), 64MB low-memory policy, AHCI/ext4 SMP fixes | `programs/installer/`, `programs/settings/`, `programs/notepad/`, `programs/desktop/`, `programs/PureDOOM/`, `lib/include/graphic/winstyle.h` |
| [scheduler.md](./scheduler.md) | Deadline-keyed scheduling plus RIP rate feedback (it only changes slice length, not CPU share), load balancing | `kernel/src/arch/x86_64/schedule/` |
| [memory.md](./memory.md) | 4-level page tables, huge pages and CoW, SLUB kernel heap, per-CPU caches, batched TLB shootdown, W^X | `kernel/src/arch/x86_64/vmm/`, `kernel/src/mem/` |
| [smp.md](./smp.md) | AP bring-up, per-CPU data segment, GS base, XSave, SMAP/SMEP | `kernel/modules/x86_64/smp/smp.cpp` |
| [syscall.md](./syscall.md) | Non-POSIX syscall ABI reference (base numbers 0-16, 25 slots registered in total) | `kernel/include/arch/x86_64/schedule/syscalln.h` |
| [gui.md](./gui.md) | CPU-parallel software compositor (N-1 workers), window layers, cursor layer, SDF corners and shadows, known flaws | `programs/desktop/` |
| [filesystems.md](./filesystems.md) | VFS / SAF / lwext4 / FAT, per-CPU file cache | `kernel/modules/common/fs/` |
| [network.md](./network.md) | e1000 82574L driver and lwIP port (DHCP/ping/multi-core TX/statistics), port fix history and known boundaries | `kernel/modules/common/drivers/net/`, `kernel/include/lwip/` |
| [dpdk-lite.md](./dpdk-lite.md) | DPDK-lite data-plane design (polling, zero-copy, offload, multi-queue) and the zero-copy audit | same as above |
| [fc-semantics.md](./fc-semantics.md) | File-cache semantics audit (writeback, dirty pages, fsync, OOM, permissions) | `kernel/modules/common/fs/fc*.cpp` |
| [goal-status.md](./goal-status.md) | Goal ledger (three-part goals, production checklist, six legacy debts) | — |
| [puredoom.md](./puredoom.md) | PureDOOM userspace port: hook wiring (malloc/file/clock/getenv), 2x blit into the WM surface, known limits (no audio, no mouse) | `programs/PureDOOM/` |
| [doom-extreme.md](./doom-extreme.md) | DOOM_TINY compression: framebuffer aliasing, dropping the RGBA copy (575KiB saved), audio stripped, WAD trimmed 11.16MB to 9.07MB, IMA-ADPCM tool at 3.93:1, running under 64MiB with zone=10, RP2040 comparison | `programs/PureDOOM/` |
| [stability-report.md](./stability-report.md) | Long-run stability report with measured data (the 30-minute soak and friends) | — |
| [stability-audit.md](./stability-audit.md) | Stability audit: fault injection and recovery paths | — |
| [release.md](./release.md) | Release process: build/host/smoke/long-run gate matrix, version conventions | — |
| [commit.md](./commit.md) | Commit message conventions | — |
| [../SECURITY.md](../SECURITY.md) | Security status (architecture-level mechanisms, explicit boundaries, known risks) and how to report a vulnerability | — |

## Design choices

SkylineSystem doesn't follow the POSIX/Linux ABI. Interfaces are designed from "what a modern desktop
kernel needs", which leads to a few trade-offs that come up over and over:

* One architecture, done properly. x86_64 only. aarch64/RISC-V/LoongArch have build parameters and
  nothing else; the ports don't exist.
* Mechanism in the kernel, policy in userspace. Window decorations, compositing policy and the shell
  live in userspace. The kernel exposes shared frames, threads and sysinfo, and stops there.
* Compositing is never blocked by a spinning client. A `for(;;)` loop in userspace doesn't stall the
  mouse or the screen, mostly because the cursor has its own layer and the compositor doesn't depend on
  client progress. The scheduler's RIP feedback only changes slice length, nothing more (scheduler.md
  spells out what it does *not* do).
