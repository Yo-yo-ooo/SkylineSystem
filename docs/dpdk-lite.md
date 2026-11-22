<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# DPDK-lite data plane (e1000e 82574L + lwIP)

The idea is to push lwIP's data plane toward the way DPDK does it: polling, per-core data structures, batching,
checksum offload. It is not a DPDK clone — there is no user-space driver framework, no hugepages, no RSS
library — just the parts that make sense inside this kernel.

## What DPDK does, and where this kernel stands

| DPDK concept | Status here | Gap / plan |
|---|---|---|
| PMD polling driver | `E1000::PollRX()` polls for packets; the lwIP thread pulls every 5ms | IRQ mode still needs INTx routing fixed (MSI-X planned) |
| Per-core, lock-free data structures | RX FIFO is single-producer single-consumer (IRQ to lwIP thread); the TX ring belongs to the lwIP thread alone | per-core pbuf pool and per-core TX queue (planned) |
| rte_mempool (preallocated pools) | lwIP `PBUF_POOL` is 32×2048, one global pool | move to per-CPU pbuf pools, see below |
| Burst API | one frame at a time | PollRX dequeues in batches, pushes to the stack in batches (planned) |
| Zero copy | no: IRQ→FIFO copy plus FIFO→pbuf copy, two copies | use RX buffers directly as pbuf pool entries (planned) |
| Checksum offload | no: software checksums both ways (TX could be offloaded, RX verification is off) | e1000e TX context descriptor offload, pending verification of the 82574 register layout |
| Multi-queue + per-core MSI-X | no: a single RX queue (82574 supports 2+2) | MRQC/RXDCTL1 plus the MSI-X table (planned, same register verification) |
| rte_ring | a plain ring FIFO, lock-free for a single producer | shape exists; multi-producer needs a CAS version |

## Per-core pbuf pool (next step)

```
per_cpu:
  pbuf_pool[MAX_CPU][PBUF_POOL_PER_CPU]     // 64×2048B per core = 128KB/core
  pbuf_free_head[MAX_CPU]                   // per-core free list, single consumer = this core
```

- Allocation takes from the local core pool; when it's empty, fall back to the global pool under a spinlock.
- Free returns the object to the local core pool, DPDK style: objects stay on the core that produced them, and a
  cross-core return is the slow path.
- lwIP change points: `PBUF_POOL` allocations have to go through a custom `pbuf_alloc` hook. `LWIP_MEM_ALIGN` and
  custom memp can't be replaced outright, so either a custom allocation macro for `PBUF_POOL` or a rewrite of the
  POOL branch in `pbuf_alloc`.

## Zero-copy receive (planned)

Current path: NIC DMA → RX buffer page → FIFO copy → pbuf copy → stack. Two copies.

Plan: make the RX buffer pages the pbuf pool pages, so PollRX attaches descriptor buffers directly as `PBUF_REF`
(a pbuf that references external memory). Once the stack has consumed them the driver reclaims the descriptor and
re-arms DMA. lwIP needs `PBUF_REF` plus a custom reclaim callback (`pbuf_free_custom`).

The risk is backpressure: while the stack holds a pbuf, the descriptor has to stay parked. With 82574's single
queue the RX stall budget is 256 descriptors × 2048B = 512KB of buffer depth, which is acceptable.

## Checksum offload (planned)

e1000e TX path: send a context descriptor (cmd=0x04, carrying IPCSS/IPCSO/TUCSS/TUCSO) followed by a data
descriptor with IFCS set; the hardware computes the IP/TCP/UDP checksums. On the lwIP side that means
`CHECKSUM_GEN_IP/UDP/TCP=0` plus the `NETIF_FLAG_..._CHECKSUM_OFFLOAD` netif flags.

Blocking: the exact field layout of the 82574 context descriptor still has to be checked against the datasheet, and
QEMU's e1000e emulation (the tso/checksum path in e1000e_core.c) needs real testing.

## Multi-queue + MSI-X (planned)

82574L has 2 RX and 2 TX queues. RX multi-queue means MRQC (0x5818) set to RSS or VT mode, RXDCTL[0/1] enabled,
queue 1's ring registers (RDBAL1, 0x2900 group) programmed, and an MSI-X table with one vector per queue routed to
different cores. Then each core runs its own PollRX thread pulling its own queue, which is a genuinely per-core
data plane. Same blocking item as checksum offload: verify the register layout, then test what QEMU actually does.

## Baseline and targets

- Today, polling with two copies under QEMU TCG: ping round-trips work, throughput has not been stress-tested yet
  (a TX flood baseline is next).
- Target path is zero-copy + batching + offload, aiming at line rate on a single core. Under TCG only the relative
  improvement is meaningful; absolute numbers need KVM or real hardware.

## Zero-copy lifetime audit

Ownership was traced segment by segment for `pbuf_alloced_custom(PBUF_RAW, len, PBUF_REF, &c->pc, data, len)`:

1. Ingress. `ethernet_input` calls `pbuf_remove_header(14)` on the REF pbuf. A REF pbuf has no header reserve,
   so the slow path allocates a new PBUF_RAM head and links the original REF behind it (ref count 1 each). The chain
   is complete.
2. Consume. `ip_input` and friends free the chain head after consuming it; walking the chain frees the REF, which
   calls `custom_free_function` → `kfree(container) + RecycleRx(idx)`. A single free is correct here — the earlier
   double free is fixed.
3. Return. RecycleRx sets state=1 and `rx_arm_advance` moves RDT forward. The lent-out hole blocks the advance.

Observation that is still unexplained: after the double-free fix, the zero-copy path still goes silent after the
first statistics print, while the copy path with the same configuration stays stable at 21 ticks per 10 minutes.
Ruled out: double free, probe hang, stack overflow, index back-derivation.

What a line-by-line re-read of the source did establish:

- `pbuf_alloced_custom`: the layer offset of PBUF_RAW is 0, so payload = data, correctly aligned.
  `LWIP_PBUF_CUSTOM_DATA_INIT` is an empty macro, so the preset `custom_free_function` is not overwritten.
- `ethernet_input`'s success path (L248) does not free the pbuf; the dispatch targets `ip_input`/`etharp` free it.
  So "free only on failure" was the right direction, and the double free came from the earlier dual-branch path.
- Raw pcb contract: `recv` returning 1 means consumed, and `ping_recv` frees itself and returns 1.
- Remaining suspects are narrow: free counting in the DHCP slow-path head chain (PBUF_RAM head plus REF chain), or
  a serial/thread interaction tied to desktop process load timing. Both need runtime tracing.

Decision: production default stays on the copy path, which is verified stable. Zero-copy remains experimental
(`NET_ZEROCOPY=0`), and the suspect list above is the follow-up work.

## What has landed

- Driver counters: rx/tx packets and bytes, loss, descriptor errors, ring full, IRQ count.
- The lwIP thread prints rx/s and tx/s every 10 seconds, so long-term trends, leaks and regressions are visible
  without extra instrumentation.
- RTT percentiles: every 5 minutes it prints ping RTT min/P50/P90/P99/max plus the loss count.
- Zero-copy with explicit index passing: pbuf_custom attached straight to DMA buffers, the check-out/RecycleRx
  state machine and the lent-out hole handling. Two things went wrong on the way and are now fixed: an index
  back-derived from an address difference (the pages are separate allocations and not contiguous, so it was wrong
  every time), and the 16KB kernel thread stack being too small for lwIP's deep paths plus DHCP frame handling,
  which overflowed and killed the thread silently — that one was misdiagnosed as a ring race for a while. The
  network thread now asks for `NewKernelThreadEx(..., 8)`.
- Stall self-diagnosis: 30 seconds with no RX frames dumps the whole ring state machine.
