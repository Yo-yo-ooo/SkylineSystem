<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# Architecture and boot flow

## 1. Layering

```
┌────────────────────────────────────────────────────────────────┐
│  userspace ELF (desktop.elf / hw.elf / helloworld)             │
│  programs/  —— window manager, console, compositor (MIT)       │
├────────────────────────────────────────────────────────────────┤
│  lib/  —— freestanding libc (printf/string/malloc/TTF)         │
│  syscall boundary (syscalln.h, 25 kernel slots)                │
├────────────────────────────────────────────────────────────────┤
│  kernel (GPL-2.0-only)                                         │
│  ├─ scheduler  schedule/  3EVDF, threads, signals, timers      │
│  ├─ memory vmm/ mem/ 4-lvl paging (5-lvl stub), PMM, SLUB, VMA │
│  ├─ concurrency  smp/  per-CPU, AP bring-up, IPI               │
│  ├─ interrupts  interrupt/  GDT/IDT/ISR, LAPIC/IOAPIC/PIC      │
│  ├─ files  fs/  VFS (fd/fc), SAF, lwext4, FAT (stub)           │
│  ├─ drivers  drivers/  NVMe/AHCI, USB (xHCI), PS/2, FB         │
│  ├─ network  net/  e1000 82574L + lwIP wired up (DHCP/ping/TCP)│
│  └─ data structs klib/algorithm/ ART, RBTree, hashmap (adapted)│
├────────────────────────────────────────────────────────────────┤
│  Limine boot protocol (BIOS + UEFI) → 64-bit direct execution  │
└────────────────────────────────────────────────────────────────┘
```

Userspace and kernel sit in separate address spaces and talk only through syscalls. That is also why
the kernel's GPLv2 does not reach the MIT code in `programs/`.

## 2. Directory layout

`kernel/src` holds the Tier 0 fixed core. Driver/FS/net/sched implementation code and the 40
component shells also live under `kernel/modules`: implementations are linked into the kernel as a
shared library, while the shells are compiled into independent `.ko` files. All 58 `.ko` files
(40 components + 18 demos) are packed into one **modules.saf** and loaded by Limine. The kernel has no
`.kbuiltin` component table any more. See [kernel-dynlink.md](kernel-dynlink.md),
[kernel-modules.md](kernel-modules.md), [kernel-components.md](kernel-components.md).

| Path | Responsibility |
|---|---|
| `kernel/src/arch/<arch>/` | Tier 0 arch core: `start.S/init.cpp`, `interrupt/` (GDT/IDT/ISR), `vmm/`, `cpu/`, `simd/`, `schedule/` (scheduler core and syscall entry), `kmod/` (loader and governance) |
| `kernel/src/mem/` | Tier 0 memory mechanism: `pmm.cpp` (physical pages), `heap.cpp` / `new.cpp` / `new2.cpp` (SLAB/SLUB) |
| `kernel/src/klib/` | Tier 0 kernel library: kprintf, serial, stallwatch, string, ART, hashmap, rbtree, queue |
| `kernel/src/elf/` | Tier 0 user ELF dynamic linker (`elf_dyn.cpp`) |
| `kernel/src/flanterm/ print/` | Tier 0 console and print backends |
| `kernel/modules/common/` | shared implementation for all arches: `acpi/`, `drivers/`, `fs/` (VFS/SAF/FAT/ext4), `net/` (lwIP), `partition/`, `smbios/`; exported to component `.ko` via `kernel.component.v1` |
| `kernel/modules/<arch>/` | arch implementation plus `components/` (KCOMPONENT shells by category -> independent `.ko`), `dev/` (PCI), `drivers/` (HPET), `lapic/ ioapic/ pit/ rtc/ smp/`, `syscall/` |
| `kernel/modules/loadable/` | 18 relocatable `.ko` demos/extensions, packed into modules.saf |
| `kernel/include/` | Shared headers: Tier 0 contracts, the kmod ABI, component interfaces |
| `programs/` | Userspace: `desktop/` (WM and compositor), `helloworld*/` |
| `lib/` | Userspace libc (`stdc/`, `graphic/`, `base/`) |
| `ablib/` | Hand-written hot libc primitives (memcpy/memset, AVX/AVX2 dispatch) |
| `res/scripts/` | QEMU launch scripts (Linux / WSL / Windows) |

`lwext4`, `fatfs`, `lwip`, `flanterm`, `limine-protocol` and `stb_truetype` are third-party ports, not
original work. lwIP is actually wired up: e1000 driver plus DHCP, ICMP ping and TCP, see
[docs/network.md](network.md).

## 3. Boot sequence

`x86_64_init()` in `kernel/src/arch/x86_64/init.cpp` runs in two layers.

Layer 1: the Tier 0 fixed core. Inline code, nothing here can be modularised.

```text
cli (external IRQs stay off during the whole component graph init)
  → Serial output ready
  → SSE
  → GDT / write KERNEL_GS_BASE / IA32_GS_MSR (per-CPU pointer)
  → IDT
  → FPU (hcf if unsupported)
  → PMM (physical pages)
  → VMM (kernel page tables, 4-level mapping; 5-level path compiled out)
  → SLAB → SLUB kmalloc (16..1024 B) → SLUB self-test
  → simd_cpu_init(0)                              ← XSave/AVX feature detection
  → enable_smep_smap()                            ← SMEP/SMAP + EFER.NXE per CPUID
  → syscall_init()                                ← MSR_LSTAR / fixed syscall entry + governance sentinels
  → KmodInit()                                    ← registry/capability/seats/syscall contract table
                                                  → KdllInit() resident core services
```

Layer 2: the DynModule graph. Limine passes one module, `boot():/boot/modules.saf`;
`KmodSafIngest()` parses the archive and registers all 58 `.ko` files in the image pool
(`kernel.*` marked trusted). `KmodBootLoad()` then loads the 43 boot modules in provider-first
order (governance auto-pulls dependencies), running bind → apply → init on each.

```text
kernel.arch.acpi → kernel.arch.apic → kernel.time.pit/hpet/rtc → kernel.fs.cache
  → kernel.arch.smp → kernel.sched.core → kernel.dev.core → kernel.pci.core
  → kernel.driver.{ahci,nvme,xhci,e1000} / kernel.input.ps2.{kbd,mouse}
  → kernel.fs.vfs → kernel.pci.enumerate → kernel.block.core → kernel.fs.ext4/fatfs
  → kernel.fs.rootfs → kernel.arch.irq → kernel.display.fb
  → kernel.sched.install → kernel.sched.policy.eevdf / kernel.alloc.policy
  → kernel.syscall.{file,proc,mem,info,time,dev,random}
  → kernel.net.netdev → kernel.net.lwip → kernel.user.init
```

Any DynModule can later be injected by name through `KdllInject()` (`kernel.dynlink.v1`);
see [kernel-dynlink.md](kernel-dynlink.md). `KMOD_SELFTEST` (14 cases) runs single-threaded
before `sti`. Then the Tier 0
tail finishes the boot:

```text
PrintFSERIAL=1 → sys_sysinfo_init() → sti
  → send SCHED_VEC IPI to BSP + other CPUs (scheduler formally takes over)
  → BSP enters an infinite hlt loop
```

### Three boot pitfalls worth remembering

1. IRQ0 must not be unmasked before the PIT handler exists. The graph therefore keeps
   `IOAPIC::RemapIRQ(IRQ0→vec32)` in its own component `kernel.arch.irq` (deps: apic, time.pit,
   arch.smp), and `x86_64_init` starts with `cli`. Otherwise a stray IRQ lands in the "uncaught IRQ →
   print" path while kprintf holds `ptf_lock`, and the recursive self-deadlock looks like a random
   boot hang.
2. The bootstrap stack gets deeper than the old hard-coded chain, because the component graph plus
   logging is deep. `start.S` installs an explicit 64 KiB `__boot_stack` in `.bss` instead of trusting
   whatever stack the bootloader left behind.
3. The init thread has to `cli` before it is enqueued. If an interrupt-driven schedule picks it right
   after enqueueing, its RIP lands on a half-filled context and the machine dies immediately.

## 4. Address spaces and security

- The kernel image takes the HHDM offset from Limine; early boot prints `HHDM OFFSET`.
- KASLR comes from the bootloader alone (`-fPIE/-pie`, randomised at load). The kernel has no
  randomisation logic of its own. After a build, `cd kernel && make kaslr-check` confirms no 32-bit
  absolute relocations are left.
- Userspace and kernel address spaces are separate. `enable_smep_smap()` turns on SMEP and SMAP (kernel
  access to user pages needs an explicit toggle) and sets EFER.NXE where CPUID allows.
- Userspace W^X is enforced after ELF loading, per segment `p_flags`: drop W on non-writable segments,
  add NX on non-executable ones.
- User/kernel copies in `ua.cpp` validate MM_USER and the 4-level user-half upper bound. Before that,
  userspace could read and write arbitrary physical memory through the HHDM alias; that's closed.
- Permission model, plainly: there is no uid/gid, no permission bits, no chmod/access. It is a
  single-user system, and sensitive operations go through the process-level trust bit `IsTrusted`
  (kill, exec, cross-process mmap). See [fc-semantics.md](fc-semantics.md) and [syscall.md](syscall.md).
