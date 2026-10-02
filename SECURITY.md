# Security Status and Vulnerability Process (SECURITY)

> Stated truthfully: this OS is an educational single-user kernel; its security mechanisms cover the
> "architecture level" rather than the "policy level". To report issues, see the process at the end.

## Implemented (Architecture Level)

| Mechanism | Location | Status |
|---|---|---|
| Kernel/user address-space isolation | VMM | ✅ |
| SMEP / SMAP | `enable_smep_smap()` | ✅ |
| NX (EFER.NXE) | enabled per CPUID | ✅ |
| User-space W^X | ELF loading tightened by p_flags | ✅ |
| KASLR | bootloader load randomization (`make kaslr-check` gate) | ✅ |
| User pointer validation | `ua.cpp` MM_USER check + half-range upper bound (a previous HHDM alias hole was fixed) | ✅ |
| syscall entry validation | out-of-range / unregistered → -ENOSYS | ✅ (round 9 audit) |
| Sensitive-operation trust bit | kill/exec/cross-process mmap require `IsTrusted` | ✅ |
| Kernel stack | 16KB default, deep-call-chain components may use 32KB | ✅ (round 14) |

## Explicit Boundaries (not defects, the current design state)

- **No uid/gid, no file permission bits, no chmod/access**: single-user OS; sensitive operations go through the process trust bit.
- **User-space ASLR is implemented on the kernel side**: the ELD loader supports ET_DYN load bias (randomized base);
  in-tree programs are currently compiled at a static base and do not enable it (P3-79 wording correction: previously written as "no user-space ASLR").
- **No security-module / CVE numbering system**: this OS does not assign CVEs; CVEs for upstream lwIP/lwext4/FatFs
  need to be tracked (see below).

## Known Risks (Technical Debt List)

1. **Kernel `_vsnprintf` hardening TODO**: the `%s` NULL guard is in place; the crashes under
   lwIP multi-module debug output were attributed to the thread stack (fixed in round 14), but vsnprintf's long-format/nested scenarios are not exhaustively covered.
2. **Zero-copy RX = experimental state** (`NET_ZEROCOPY=0` off by default), see docs/dpdk-lite.md.
3. **sched_bench not verified on the real kernel** (model tests pass).
4. Upstream component version tracking: CVE advisories for lwIP / lwext4 / FatFs must be periodically
   compared against the vendored versions in `kernel/src/net` and `kernel/src/fs`.

## Vulnerability Reporting Process

1. Open an issue in the project repository with `[SECURITY]` in the title, including reproduction steps (QEMU flags + logs).
2. Issues involving upstream (lwIP/lwext4/FatFs) are forwarded upstream with the forwarding reference recorded.
3. After the fix is merged, update the "Known Risks" section of this document and docs/stability-audit.md.

## Upstream CVE Tracking List (C6, round 15)

> Minimum-cost manual tracking while automation is absent: each quarter, check the versions below
> line by line against NVD advisories. Automation direction (optional): a GitHub Action periodically
> pulls NVD's cpeMatch, matches the versions pinned in this table, and opens an issue on a hit.

| Component | vendored location | pinned version | last checked | TODO |
|---|---|---|---|---|
| lwIP | kernel/src/net | bundled in repo (upstream tag not recorded) | from round 15 | record the upstream tag |
| lwext4 | kernel/src/fs/lwext4 | bundled in repo | from round 15 | record the upstream tag |
| FatFs | kernel/src/fs/fatfs | bundled in repo | from round 15 | record the upstream tag |
| mpaland/printf | lib/stdc/outfb/printf.c | upstream MIT version | from round 15 | track upstream fixes |
| x86mem | ablib/arch/x86_64/x86mem | developed in this repo (MIT) | — | none |
