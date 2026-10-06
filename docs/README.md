# SkylineSystem documentation hub

This directory holds SkylineSystem's **in-depth design documentation**. The root [README.md](../README.md) covers "what it is, how to build it, how to run it"; this directory covers "how its internals are actually designed, and why they are designed that way".

> All documents are written from 
>
> `kernel/`
>
>  the actual source code, not marketing copy. Whenever comments like 
>
> `v3 FIX`
>
>  appear, it means this area genuinely hit a pitfall historically.

## Documentation map



| Document                                     | Contents                                                                                                                            | Corresponding code                                |
| -------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------- |
| [architecture.md](./architecture.md)         | Overall layering, directory layout, boot sequence from Limine to the first ELF                                                      | `kernel/src/arch/x86_64/init.cpp`                 |
| [scheduler.md](./scheduler.md)               | deadline-keyed scheduling + **RIP rate feedback** (only adjusts time-slice length, **does NOT change CPU share**), load balancing   | `kernel/src/arch/x86_64/schedule/`                |
| [memory.md](./memory.md)                     | 4-level page tables, huge pages + CoW, SLUB kernel heap, per-CPU caches, batched TLB shootdown, W^X                                 | `kernel/src/arch/x86_64/vmm/`, `kernel/src/mem/`  |
| [smp.md](./smp.md)                           | AP bring-up, per-CPU data segment, GS base, XSave, SMAP/SMEP                                                                        | `kernel/src/arch/x86_64/smp/smp.cpp`              |
| [syscall.md](./syscall.md)                   | non-POSIX syscall ABI reference (base numbers 0-16; the kernel registers 25 slots in total)                                         | `kernel/include/arch/x86_64/schedule/syscalln.h`  |
| [gui.md](./gui.md)                           | CPU-parallel software compositor (N-1 workers), window layers, cursor layer, SDF rounded corners and shadows, including known flaws | `programs/desktop/`                               |
| [filesystems.md](./filesystems.md)           | VFS / SAF / lwext4 / FAT, per-CPU file cache                                                                                        | `kernel/src/fs/`                                  |
| [network.md](./network.md)                   | e1000 82574L driver + lwIP port (DHCP/ping/multi-core TX/statistics), port-fix history and known boundaries                         | `kernel/src/drivers/net/`, `kernel/include/lwip/` |
| [dpdk-lite.md](./dpdk-lite.md)               | DPDK-lite data-plane design (polling/zero-copy/offload/multi-queue) + zero-copy audit                                               | same as above                                     |
| [fc-semantics.md](./fc-semantics.md)         | complete file-cache semantics audit (writeback/dirty pages/fsync/OOM/permissions)                                                   | `kernel/src/fs/fc*.cpp`                           |
| [goal-status.md](./goal-status.md)           | goal-completion ledger (three-part goals + production-grade eight pillars + six legacy debts)                                       | —                                                 |
| [puredoom.md](./puredoom.md)                 | PureDOOM userspace port: hook wiring (malloc/file/clock/getenv), 2x blit into the WM surface, known limits (no audio/mouse)        | `programs/PureDOOM/`                              |
| [stability-report.md](./stability-report.md) | long-run stability report (measured data such as the 30-minute soak)                                                                | —                                                 |
| [stability-audit.md](./stability-audit.md)   | stability audit (fault injection / recovery paths)                                                                                  | —                                                 |
| [release.md](./release.md)                   | release process (build/host/smoke/long-run gate matrix + version conventions)                                                       | —                                                 |
| [commit.md](./commit.md)                     | commit message conventions (existing)                                                                                               | —                                                 |
| [../SECURITY.md](../SECURITY.md)             | security status (architecture-level mechanisms / explicit boundaries / known risks) and vulnerability reporting process             | —                                                 |

## Design philosophy in brief

SkylineSystem does **not** chase the POSIX/Linux ABI; every interface is designed from scratch around "what a modern desktop kernel should look like". Core trade-offs:



* **Depth first**: one architecture, x86\_64, done through and through; aarch64/RISC-V/LoongArch have build parameters only, **ports not implemented**.

* **Mechanism/policy separation**: window decorations, compositing policy and the shell all live in userspace; the kernel only exposes minimal mechanisms such as shared frames, threads and sysinfo.

* **Preemptibility first**: even if userspace spins in a `for(;;)` loop, the mouse and screen compositing stay smooth — guaranteed mainly by the **independent cursor layer** and the compositor structure; the scheduler's RIP rate feedback only adjusts time-slice length (see the honest description in scheduler.md).
