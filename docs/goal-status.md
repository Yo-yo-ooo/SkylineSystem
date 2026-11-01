# Goal status and verification ledger

> The goal has three parts: (A) e1000 driver + (B) lwIP porting completion + (C) DPDK-style high-performance rework,
> plus the production-grade checklist (SMP concurrency / long-term stability / fault injection / security / full semantics / performance profiling /
> regression gating / observability). Checked item by item, annotated truthfully.

## 🏁 Milestone: four-source audit fix list —— ✅ all cleared

All six tiers cleared: **P0 ×18** (including two convergence items that were reverted and redone: the TLB ACK fence —
root cause of the 400M pause spins under TCG → capped at 100ms wall clock, measured 0 timeouts; and the AHCI
per-request DMA buffer that replaced the lock spanning the sti window), **P1 ×36**, **P2 ×8** (cross-core ABA
isolation stack foreign_head, AHCI release barrier, QSBR 64 slots), **P3 ×21**, **P4 ×8** (allocator host chaos
test — which also caught a real MCB allocation-granularity bug; fc writeback content verification), **P5 ×19**.
Verification after each change set: BUILD_EXIT=0 + GOLDEN PASS + QEMU ping 3 + 0 exceptions.

## 🏁 Roadmap convergence

| # | Roadmap item | Status |
|---|---|---|
| #7 | FD inheritance (filedesc snapshot-style deep copy) | ✅ also confirmed two latent defects (rb_postorder_iter callback contract + exec FDMan release not nulling) |
| #2 | proc lifecycle TOCTOU | ✅ ProcessAddThread re-check inside lock + reaper FDMan reclamation into lock (5 call sites closed) |
| #6 | vsnprintf fuzz | ✅ ASAN host fuzz + real out-of-bounds read fix (NUL-terminated format string) |
| — | fsync production activation | ✅ fclose + implicit dirty-page flush at process exit |
| — | Sleep/timer wheel revived | ✅ PIT::Sleep dual mode (scheduler ready → real sleep) |
| — | RX burst observability | ✅ rx_burst_avg (DPDK metric) |
| — | Static analysis | ✅ clang --analyze 9 files + 2 defensive fixes (fc LRU double-advance + rbtree sibling guard) |

## A. e1000 driver —— ✅ done

- 82574L (QEMU e1000e) complete driver: probe/reset/EEPROM MAC/dual ring 256/IRQ+polling dual mode
- Bug-fix history: RDT ownership, _memcpy transposed order, GetPhysics in-page offset,
  pbuf double free, PIT::Sleep hang during probe, early PIT busy-wait guard
- Multi-core TX spinlock serialization + ring-full backoff

## B. lwIP porting —— ✅ done (production default: polling + copy path)

- End-to-end verification: DHCP lease (10.0.2.15), ICMP ping consecutive replies, multi-core TX flood 2.2-2.7k frames/s
- Porting fixes: lwipopts (software checksum/alignment/pool), cc.h DIAG varargs chain fix,
  sys_now monotonic milliseconds, ACD disabled (slirp ARP proxy), ip4addr_ntoa static-buffer pitfall
- Network thread stack 32KB (16KB stack overflow = one of the deep root causes of all intermittent crashes)

## C. DPDK-lite —— 🟡 base complete, two experimental items documented

| DPDK concept | Status |
|---|---|
| PMD polling | ✅ production path |
| Multi-core TX concurrency | ✅ spinlock + 3-core flood verification |
| Per-core private / lock-free | 🟡 single RX queue (dual queue = MSI-X blocking item) |
| Zero-copy RX | 🟡 experimental (NET_ZEROCOPY=0): index back-derivation + double free fixed, still silent after first stat (suspects narrowed to 2 items, see dpdk-lite.md) |
| Checksum offload / MSI-X | ❌ blocked on register-manual verification |
| Stats/observability | ✅ counters + RTT P50/P90/P99 + stall self-diagnosis |

## Production-grade checklist item by item

| Pillar | Status | Evidence |
|---|---|---|
| SMP concurrency correctness | ✅ existing (SLUB/FC/sched multithreaded suites) + network multi-core stress + **110-item P0/P2 concurrency fixes all cleared** | tests/ + soak |
| Long-term stability | ✅(evidence) closing soak chain: 10/15/30min + 40min multi-segment all 0 assertions 0 exceptions 0 stalls; final 30min: tx=1.29M frames (82.7MB) sustained at 2429/s, ping=185 consecutive, loss=0; **re-tested after every milestone** | serial.log |
| Fault injection | ✅ OOM/page allocation/disk errors (newly tested)/RX OOM injection all closed-loop | fc_reg + net injection |
| Security audit | 🟡 syscall entry audit ✅, SECURITY.md ✅, trust-bit model documented; fuzzing/defense-in-depth ❌ | SECURITY.md |
| Full semantics | ✅ writeback/dirty-page/fsync/OOM semantics audit (fc-semantics.md); permissions = trust bits (as-is) | fc-semantics.md |
| Performance profiling | ✅ RTT percentiles + sched_bench real-kernel report (step/osc pass; pollute=real finding, shortwin=semantic mismatch) | scheduler.md |
| Regression gating | ✅ three-tier gating: host CI + golden diff + QEMU network smoke | tests/ |
| Observability | ✅ counters/rates/RTT/panic to serial/stall self-diagnosis | per-change verification |

## Remaining technical debt (by priority)

| # | Debt | Status | Fix design |
|---|---|---|---|
| 1 | pollute phase RIP multiplier residual variance | 3x convergence achieved (accumulator), full pass awaiting in-window instrumentation | numerator-side / phase-transition instrumentation |
| 2 | exit reclamation path spinlock UAF | lock pairing converged (TOCTOU window closed); full cure still leaves proc refcounting | proc refcounting (cure) |
| 3 | Zero-copy remaining suspects | see dpdk-lite.md (suspects narrowed to 2 items) | pbuf lifetime runtime tracking |
| 4 | sched_bench shortwin sampling | switch-point sampling already online (stale docs fixed) | ✅ converged |
| 5 | Checksum offload / MSI-X multi-queue | blocked on 82574 datasheet | implement after register verification |
| 6 | Kernel vsnprintf deep hardening + fuzzing in depth | fuzz + out-of-bounds read fix ✅ | ✅ converged |
| 7 | FD inheritance | fd_manager_dup ✅ (lock/OOM rollback included) | ✅ converged |
| 8 | xHCI async IN path | sync path fixed; async via bounce/SG to be implemented (blocked on real-hardware verification) | per-page chained TRB extended to async |
| 9 | Audit latest batch | High-risk 14 items: 11 fixed + 3 false positives; medium-risk 13 items: 6 fixed + 5 not applicable/already covered + allocator integration analysed (needs boot-order coordination) + delayed detection is by design (SLAB detects at refill, SLUB immediate); design debt analysed (semantics correct, the rename that caused a boot hang in testing was reverted, migration plan on record); hygiene 16 items: 15 closed (incl. 3 outdated claims: fb.cpp copy init / x86mem MIT added / libc line numbers pointed at wrong file) + 1 roadmap item (-Wno batched removal) | see the audit records |
