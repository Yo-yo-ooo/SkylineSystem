# Overall architecture and boot flow

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

Userspace and kernel run in **separate address spaces** and communicate only through syscalls, so the kernel's GPLv2 does not infect the userspace MIT code.

## 2. Directory layout (full modularization)

`kernel/src` now holds **only Tier0 fixed core**; every component implementation
(drivers/filesystems/network/partition/platform/syscall families) lives under
`kernel/modules`. Both trees use the same private-contract component ABI and
governance (see [kernel-modules.md](kernel-modules.md), [kernel-components.md](kernel-components.md)).

| Path                         | Responsibility                                                                                                                                            |
| ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `kernel/src/arch/<arch>/`    | Tier0 architecture core: `start.S/init.cpp`, `interrupt/` (GDT/IDT/ISR), `vmm/`, `cpu/`, `simd/`, `schedule/` (scheduler core + syscall entry), `kmod/` (loader/governance) |
| `kernel/src/mem/`            | Tier0 memory mechanism: `pmm.cpp` (physical pages), `heap.cpp` / `new.cpp` / `new2.cpp` (SLAB/SLUB)                                                       |
| `kernel/src/klib/`           | Tier0 kernel library: kprintf/serial/stallwatch/string/ART/hashmap/rbtree/queue                                                                          |
| `kernel/src/elf/`            | Tier0 user ELF dynamic linker (`elf_dyn.cpp`)                                                                                                             |
| `kernel/src/flanterm/ print/`| Tier0 console/print backends                                                                                                                             |
| `kernel/modules/builtin/common/` | Tier1 builtin components (all arches): `acpi/`, `drivers/`, `fs/` (VFS/SAF/FAT/ext4), `net/` (lwIP), `partition/`, `smbios/`                        |
| `kernel/modules/builtin/<arch>/` | Tier1 builtin components (arch-specific): `components/` (KCOMPONENT shells by category), `dev/` (PCI), `drivers/` (HPET), `lapic/ ioapic/ pit/ rtc/ smp/`, `syscall/` (impl families) |
| `kernel/modules/loadable/`   | Tier2 loadable `.ko` demo modules (compiled by `make -C kernel modules`)                                                                                  |
| `kernel/include/`            | shared headers: Tier0 contracts, kmod ABI, and component interface headers                                                                                |
| `programs/`                  | userspace: `desktop/` (WM + compositor), `helloworld*/`                                                                                                   |
| `lib/`                       | userspace libc (`stdc/`, `graphic/`, `base/`)                                                                                                             |
| `ablib/`                     | hand-written high-frequency libc primitives (memcpy/memset, AVX/AVX2 dispatch)                                                                            |
| `res/scripts/`               | QEMU launch scripts (Linux / WSL / Windows)                                                                                                               |

> `lwext4`, `fatfs`, `lwip`, `flanterm`, `limine-protocol`, `stb_truetype` are third-party ports/borrowings, not original work. lwIP is wired up (e1000 driver + DHCP/ICMP ping/TCP, see docs/network.md).

## 3. Boot sequence

`x86_64_init()` (`kernel/src/arch/x86_64/init.cpp`) now runs in two layers:

**Layer 1 — Tier0 fixed core (inline, cannot be modularized):**

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
```

**Layer 2 — Tier1 builtin component graph, initialized by `KmodBootComponents()`**
(source under `kernel/modules/builtin/`, manifests declare provider-first dependencies;
`builtin.cpp` scans `.kbuiltin.desc`, Kahn-sorts, then bind→apply→init each component):

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

Then `KmodBootIngest()` registers the loadable `.ko` images from the Limine module
list and `KmodBootLoad()` loads `KMOD_MF_BOOT` ones (Tier2 — `modules/loadable/`,
same topology graph as builtins). The in-kernel `KMOD_SELFTEST` (11 cases) runs
**single-threaded before sti**, then the Tier0 tail does:

```text
PrintFSERIAL=1 → sys_sysinfo_init() → sti
  → send SCHED_VEC IPI to BSP + other CPUs (scheduler formally takes over)
  → BSP enters an infinite hlt loop
```

### Three easy-to-hit boot pitfalls (noted in source comments)

1. **IRQ0 must not be unmasked before the PIT handler is registered**. The graph
   therefore keeps `IOAPIC::RemapIRQ(IRQ0→vec32)` in its own component
   `kernel.arch.irq` (deps: apic + time.pit + arch.smp), and `x86_64_init` starts
   with `cli`; otherwise a stray IRQ enters the "uncaught IRQ → print" path while
   kprintf holds `ptf_lock` → recursive self-deadlock (silent random boot hang).
2. **Bootstrap stack depth**: the component graph + logging path is deeper than
   the old hard-coded chain; `start.S` installs an explicit 64 KiB `__boot_stack`
   in `.bss` instead of relying on the bootloader stack size.
3. **The init thread must cli before being enqueued**. Otherwise, right after
   enqueueing, an interrupt-driven schedule picks a thread that has not finished
   initializing, and its RIP lands on a half-filled context and crashes immediately.

## 4. Address spaces and security

- The kernel image gets the HHDM (Higher Half Direct Map) offset from Limine; early boot prints `HHDM OFFSET`.
- **KASLR is provided by the bootloader only** (built with `-fPIE/-pie`, load-time randomization); the kernel itself has no randomization logic. After building you must run `cd kernel && make kaslr-check` to confirm no 32-bit absolute relocations remain.
- Userspace/kernel address spaces are isolated; `enable_smep_smap()` turns on SMEP and SMAP (kernel access to user pages requires explicit toggling) and enables **EFER.NXE** per CPUID.
- Userspace **W^X**: after ELF loading, mappings are tightened per segment `p_flags` (drop W on non-writable segments, add NX on non-executable segments).
- User/kernel copies (`ua.cpp`) enforce `MM_USER` validation and the 4-level user-half upper bound — there used to be a "userspace reads/writes arbitrary physical memory through the HHDM alias" vulnerability, now fixed.
- **Honest description of the permission model**: no uid/gid, no file permission bits, no chmod/access (single-user OS); sensitive operations go through the process-level trust bit `IsTrusted` (kill/exec/cross-process mmap, etc.). See [fc-semantics.md](fc-semantics.md) and [syscall.md](syscall.md).
