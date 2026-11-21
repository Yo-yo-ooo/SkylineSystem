# Security status and how to report a bug

SkylineSystem is a single-user hobby kernel. What exists is architecture-level hardening, not a
policy layer: no uid/gid, no file permission bits. Anything that reports an issue should follow the
process at the end of this file.

## What is in place

| Mechanism | Where | State |
|---|---|---|
| Kernel/user address-space separation | VMM | yes |
| SMEP / SMAP | `enable_smep_smap()` | yes |
| NX (EFER.NXE) | set when CPUID allows | yes |
| User-space W^X | ELF loading checks `p_flags` | yes |
| KASLR | bootloader randomises the load base (`make kaslr-check` gates it) | yes |
| User pointer validation | `ua.cpp` MM_USER check plus an upper bound on the half range (an earlier HHDM alias hole was closed) | yes |
| syscall entry validation | out-of-range or unregistered slot returns -ENOSYS | yes (round 9 audit) |
| Trust bit for sensitive operations | kill/exec/cross-process mmap all require `IsTrusted` | yes |
| Kernel stack | 16KB by default; deep call chains may take 32KB | yes (round 14) |

### 2026-11 user-pointer sweep

`SmapGuard` only toggles AC so the kernel may touch user pages; it does not check
that the address *is* a user address. Several handlers dereferenced caller
pointers directly under `SmapGuard`, which gave any process an arbitrary
kernel-address read/write. Fixed this round:

- `SYSCALL_TIME` / `gettimeofday` / `settimeofday` / `clock_gettime`
  (`kernel/modules/x86_64/syscall/time.cpp`): all copies go through
  `VMM::UserAccess::CopyToUser/CopyFromUser`; the dead
  `pv & MM_READ & MM_WRITE` check and the zero-usecond `gettimeofday` were
  replaced with real timeval/timespec construction.
- `SYSCALL_KMOD_INFO` (29) (`kernel/src/arch/x86_64/kmod/syscall_registry.cpp`):
  the module-table snapshot now uses `CopyToUser`; an unwritable buffer returns
  `-EFAULT` instead of writing to the raw pointer.
- `SYSCALL_GETRANDOM` (`kernel/modules/x86_64/syscall/random.cpp`):
  fills a 256-byte kernel buffer per chunk and copies it out; calls above
  16 MiB return `-EINVAL`.

The ELF loader also rejects relocations and symbols that target sections skipped
by the layout pass (`SHT_NOTE`/toolchain metadata): such a section has
`sec_off == 0`, so the old bounds check degenerated to the attacker-controlled
`sh_size` and produced an out-of-image write. Regression coverage:
`tests/kmod` case `reloc target SHT_NOTE`. Remaining known gaps are listed
below (loader module code still runs in ring 0, no W^X for module images).

`CopyFromUser` now takes `vma_lock -> pt_lock` like `CopyToUser`, closing the
page-table TOCTOU between a caller's check and the copy. The page-fault
diagnostic path uses `CopyFromUserNoLock` because it may already run inside a
holder of those locks.

Module images of seat providers are intentionally not freed on unload/hot
replace (`KMOD_MODF_SEAT_PROVIDER`): the scheduler and heap call seat ops
without entering the provider's epoch, so freeing the code they may still be
executing is a use-after-free. The trade-off is one leaked image (tens of KB)
per swap; module unload still returns success and the seat reverts to the
kernel default.

Two interrupt/return-path hardening changes landed with this sweep:

- `syscall_handler` rejects a user RCX outside the user half before dispatch.
  `sysret` would load that value into RIP and take a #GP at CPL0 (hcf); the
  non-canonical value can only come from a user program, so the process is
  terminated instead.
- `#DF`(8)/`NMI`(2)/`#MC`(18) now use the per-CPU IST1 stack that was already
  allocated and published in the TSS. A double fault caused by a corrupted
  kernel stack can now enter the C handler on a fresh stack instead of
  triple-faulting.

## Boundaries

These are design decisions, not bugs waiting to be fixed.

- No uid/gid, no permission bits, no chmod/access. Single-user system. Sensitive operations are
  gated by the per-process trust bit instead.
- User-space ASLR is supported on the kernel side. The ELF loader handles ET_DYN load bias; the
  in-tree programs are linked at a static base and leave it off. An earlier version of this file said
  "no user-space ASLR", which was wrong (P3-79 wording correction).
- No security module, no CVE numbering of our own. CVEs against vendored lwIP/lwext4/FatFs have to
  be tracked by hand, see the watch list below.

## Known risks

1. `_vsnprintf` hardening is unfinished. The `%s` NULL guard is in place; the crashes seen while
   debugging lwIP multi-module output turned out to be the thread stack (fixed in round 14). Long and
   nested format strings are still not covered exhaustively.
2. Zero-copy RX is experimental, `NET_ZEROCOPY=0` by default. See docs/dpdk-lite.md.
3. `sched_bench` has only been run against the model, never against the real kernel.
4. Vendored component versions are not pinned to upstream tags, so advisories cannot be matched
   mechanically.
5. Module images are loaded into ordinary kernel heap (`kmod_alloc`) and mapped RWX: there is no
   per-section page protection for `.ko` text/rodata/data, and no W^X for module code. Anything that
   can load a module can write its own code and the kernel's writable data; this is the same trust
   level as a Linux kernel module, not a sandbox.
6. `KdllResolve(name, sym)` returns a raw address without pinning the provider. It is only safe while
   the caller also holds a `KMOD_INJ_PIN` (or a consumer import edge) on that module; otherwise an
   unload/hot-replace can free the code behind the pointer.

## Reporting a vulnerability

1. Open an issue titled `[SECURITY] ...`, with reproduction steps, the QEMU flags used and the serial
   log attached.
2. If the problem is upstream (lwIP/lwext4/FatFs), forward it upstream and record the reference in the
   issue.
3. Once the fix is merged, update this file and docs/stability-audit.md.

## Upstream CVE watch list (C6, round 15)

Checked by hand every quarter until something automatic exists: go through the versions below against
NVD advisories line by line. The automation direction, if it ever happens, is a GitHub Action that
pulls NVD `cpeMatch`, compares it with the versions pinned here and opens an issue on a hit.

| Component | vendored location | pinned version | last checked | TODO |
|---|---|---|---|---|
| lwIP | kernel/src/net | bundled, upstream tag not recorded | round 15 | record the upstream tag |
| lwext4 | kernel/modules/common/fs/lwext4 | bundled | round 15 | record the upstream tag |
| FatFs | kernel/modules/common/fs/fatfs | bundled | round 15 | record the upstream tag |
| mpaland/printf | lib/stdc/outfb/printf.c | upstream MIT version | round 15 | follow upstream fixes |
| x86mem | ablib/arch/x86_64/x86mem | written in this repo (MIT) | — | none |
