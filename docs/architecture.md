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

## 2. Directory layout (original parts)

| Path                         | Responsibility                                                                                                                                            |
| ---------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `kernel/src/arch/x86_64/`    | architecture-specific: `schedule/`, `vmm/`, `smp/`, `interrupt/`, `lapic/`, `ioapic/`, `pci/`, `drivers/`                                                 |
| `kernel/src/mem/`            | `pmm.cpp` (physical pages), `heap.cpp` / `new.cpp` / `new2.cpp` (SLUB)                                                                                    |
| `kernel/src/klib/algorithm/` | `art.c` (adaptive radix tree, **adapted from libart/MIT, attribution restored**), `hashmap.c` (**adapted from tidwall/hashmap.c/MIT**), `rbtree`, `queue` |
| `kernel/src/fs/`             | `fc.cpp` (file cache), `fd.cpp` (file descriptors), `saf/`, `fatfs/`, `lwext4/`                                                                           |
| `kernel/include/`            | header tree mirroring `src`                                                                                                                               |
| `programs/`                  | userspace: `desktop/` (WM + compositor), `helloworld*/`                                                                                                   |
| `lib/`                       | userspace libc (`stdc/`, `graphic/`, `base/`)                                                                                                             |
| `ablib/`                     | hand-written high-frequency libc primitives (memcpy/memset, AVX/AVX2 dispatch)                                                                            |
| `res/scripts/`               | QEMU launch scripts (Linux / WSL / Windows)                                                                                                               |

> `lwext4`, `fatfs`, `lwip`, `flanterm`, `limine-protocol`, `stb_truetype` are third-party ports/borrowings, not original work. lwIP is wired up (e1000 driver + DHCP/ICMP ping/TCP, see docs/network.md).

## 3. Boot sequence

The entry point `x86_64_init()` (`init.cpp`) runs on the BSP in a fixed order, with each step checkpointed by `InitFunc(name, expr)`:

```text
Serial output ready
  → SSE
  → GDT / write KERNEL_GS_BASE / IA32_GS_MSR (per-CPU pointer)
  → IDT
  → FPU (hcf if unsupported)
  → PMM (physical pages)
  → VMM (kernel page tables, 4-level mapping; 5-level path compiled out)
  → SLAB → SLUB kmalloc (16..1024 B) → SLUB self-test
  → ACPI / MADT
  → mask the 8259 PIC (outb 0xff to 0x21/0xa1), enable ICMR
  → LAPIC / IOAPIC
  → PIT & RTC / HPET
  → BSP per-CPU file cache file_cache
  → smp_init()                                    ← bring up all APs (see smp.md)
  → IOAPIC::RemapIRQ(0→vec32)                     ← redirect PIT GSI0 to vector 32
  → RTC
  → simd_cpu_init(0)                              ← XSave/AVX feature detection
  → enable_smep_smap()                            ← SMEP/SMAP + enable EFER.NXE per CPUID
  → Schedule::Init()
  → InitCPUThread()                               ← create the init thread as the ancestor of all processes
  → syscall_init()                                ← install MSR_LSTAR / syscall entry
  → Dev / PCI enumeration (use ECAM if MCFG is present, otherwise DoPCIWithoutMCFG)
  → AHCI driver
  → PS/2 mouse / keyboard
  → ext4 mounted on "sata0" → /mp/
  → FrameBufferDevice::Init()
  → Schedule::Install()                           ← install the idle thread; the scheduler formally takes over
  → sys_sysinfo_init()
  → NewProcess + NewThread("/mp/desktop.elf")     ← desktop
  → NewProcess + NewThread("/mp/hw.elf")          ← demo program
  → sti; send SCHED_VEC IPI to the BSP and the other CPUs to trigger a schedule immediately
  → the BSP itself enters an infinite hlt loop
```

### Three easy-to-hit boot pitfalls (noted in source comments)

1. **PIT not redirected = frozen system**. After the 8259 PIC is masked, unless GSI0 is redirected to vector 32 through the IOAPIC, PIT interrupts never arrive → `TimeSinceBootMS()` freezes → EEVDF's vruntime, quantum feedback and all timeouts stop, manifesting as an ~14-second boot stall and a frozen mouse/compositor at runtime.
2. **An AP that receives a scheduling tick before idle_thread is ready will spin idly**. `Schedule::Switch` early-returns when `idle_thread/current_thread` are null: it re-arms the oneshot timer and EOIs directly, never dereferencing a null pointer.
3. **The init thread must cli before being enqueued**. Otherwise, right after enqueueing, an interrupt-driven schedule picks a thread that has not finished initializing, and its RIP lands on a half-filled context and crashes immediately.

## 4. Address spaces and security

- The kernel image gets the HHDM (Higher Half Direct Map) offset from Limine; early boot prints `HHDM OFFSET`.
- **KASLR is provided by the bootloader only** (built with `-fPIE/-pie`, load-time randomization); the kernel itself has no randomization logic. After building you must run `cd kernel && make kaslr-check` to confirm no 32-bit absolute relocations remain.
- Userspace/kernel address spaces are isolated; `enable_smep_smap()` turns on SMEP and SMAP (kernel access to user pages requires explicit toggling) and enables **EFER.NXE** per CPUID.
- Userspace **W^X**: after ELF loading, mappings are tightened per segment `p_flags` (drop W on non-writable segments, add NX on non-executable segments).
- User/kernel copies (`ua.cpp`) enforce `MM_USER` validation and the 4-level user-half upper bound — there used to be a "userspace reads/writes arbitrary physical memory through the HHDM alias" vulnerability, now fixed.
- **Honest description of the permission model**: no uid/gid, no file permission bits, no chmod/access (single-user OS); sensitive operations go through the process-level trust bit `IsTrusted` (kill/exec/cross-process mmap, etc.). See [fc-semantics.md](fc-semantics.md) and [syscall.md](syscall.md).
