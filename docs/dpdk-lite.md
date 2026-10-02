# DPDK-lite network data plane design (e1000e 82574L + lwIP)

> Goal: reshape lwIP's data plane into a "DPDK-like" high-performance form —— polling, per-core private
> data structures, batch processing, checksum offload. **Not a DPDK clone** (no user-space driver framework /
> no hugepage / no RSS library), but landing DPDK's core ideas onto this kernel architecture.

## DPDK concepts → this-kernel mapping

| DPDK concept | This-kernel status | Gap/plan |
|---|---|---|
| **PMD polling driver** | ✅ `E1000::PollRX()` polls for packets (lwIP thread pulls every 5ms) | in IRQ mode still need to fix INTx routing (MSI-X planned) |
| **Per-core private data structures (lock-free)** | ⚠️ RX FIFO single-producer single-consumer (IRQ→lwIP thread); TX exclusively owned by lwIP thread | per-core pbuf pool + per-core TX queue (planned) |
| **rte_mempool (preallocated object pool)** | ⚠️ lwIP PBUF_POOL 32×2048 (global) | switch to per-CPU pbuf pool (see below) |
| **Batched send/receive (burst API)** | ❌ single-frame processing | PollRX batch dequeue → batch push to stack (planned) |
| **Zero copy** | ❌ IRQ→FIFO copy + FIFO→pbuf copy (2 copies) | receive buffers directly as pbuf pool entries (planned, see below) |
| **Checksum offload** | ❌ software checksum (TX side fully on, RX side verification off) | e1000e TX context descriptor offload (needs 82574 datasheet register-layout verification) |
| **Multi-queue + MSI-X per-core** | ❌ single RX queue (82574 supports 2+2) | MRQC/RXDCTL1 + MSI-X table (planned, also needs register verification) |
| **rte_ring (lock-free ring queue)** | ⚠️ simple ring FIFO (spinlock-free single producer) | initial shape exists; multi-producer needs CAS version |

## Per-core pbuf pool design (next step)

```
per_cpu:
  pbuf_pool[MAX_CPU][PBUF_POOL_PER_CPU]     // per core 64×2048B = 128KB/core
  pbuf_free_head[MAX_CPU]                    // per-core free list (single consumer = this core)
```
- Allocation: take from local core pool; empty → global pool (spinlock).
- Free: **return to the local core pool** (DPDK style: objects stay on the core that produced them, cross-core return takes the slow path).
- lwIP change points: `PBUF_POOL` allocations go through a custom `pbuf_alloc` hook (`LWIP_MEM_ALIGN` +
  custom memp cannot be replaced directly —— use a custom allocation macro for `PBUF_POOL` or change `pbuf_alloc`'s
  POOL branch).

## Zero-copy receive (planned)

Current path: NIC DMA → RX buffer page → FIFO copy → pbuf copy → protocol stack (2 copies).
Zero-copy plan: **RX buffer pages are the pbuf pool pages** —— PollRX directly attaches descriptor buffers as
`PBUF_REF` (a pbuf referencing external memory); after the protocol stack consumes them the driver reclaims the descriptor and re-arms
DMA. lwIP needs PBUF_REF + a custom reclaim callback (`pbuf_free_custom`).
Risk: while the protocol stack holds the pbuf the descriptor must be parked (backpressure); under 82574 single queue the
RX stall cost = 256 descriptors × 2048B = 512KB buffer depth, acceptable.

## Checksum offload (planned)

e1000e TX path: send a **context descriptor** (cmd=0x04, containing IPCSS/IPCSO/TUCSS/TUCSO)
+ a data descriptor (cmd with IFCS); hardware computes IP/TCP/UDP checksums. lwIP side:
`CHECKSUM_GEN_IP/UDP/TCP=0` + netif flags `NETIF_FLAG_..._CHECKSUM_OFFLOAD`.
Blocking item: the exact field layout of the 82574 context descriptor needs cross-checking against the datasheet / kernel source;
QEMU e1000e emulation support (e1000e_core.c's tso/checksum path) pending real testing.

## Multi-queue + MSI-X (planned)

82574L: 2 RX + 2 TX queues. RX multi-queue = MRQC (0x5818) selects RSS or VT mode +
RXDCTL[0/1] enable + queue1 ring registers (RDBAL1 0x2900 group) + MSI-X table with one vector per queue
routed to different cores. Then one PollRX thread per core each pulling its own queue —— a truly per-core independent
data plane. Blocking item same as checksum offload: register-layout verification + QEMU emulation behavior testing.

## Performance baseline and targets

- Current (polling + double copy, QEMU TCG): ping round-trip normal; throughput not yet stress-tested (add TX flood baseline next round).
- Target path: zero-copy + batching + offload → target "single-core 100Mbps line-rate direction" (under TCG
  judge by relative improvement; absolute numbers only hold on KVM/real hardware).

## Zero-copy lifetime audit (finalized round 20)

Ownership chain analyzed segment by segment (`pbuf_alloced_custom(PBUF_RAW, len, PBUF_REF, &c->pc, data, len)`):

1. **Ingress**: `ethernet_input` applies `pbuf_remove_header(14)` to the REF pbuf —— REF has no header reserve →
   slow path allocates a new PBUF_RAM head + links the original REF (ref 1 each) → chain complete.
2. **Consume**: `ip_input` etc. free the chain head after consuming → chain walk frees the REF → triggers `custom_free_function`
   → `kfree(container) + RecycleRx(idx)`. **Single free is correct** (double free fixed in round 17).
3. **Return**: RecycleRx sets state=1 + `rx_arm_advance` advances RDT forward; the lent-out hole blocks the advance ✓.

**Still unexplained observation** (round 19 bisect): after the double-free fix the zero-copy path still goes "silent after first stat"
(copy path with same config stable at 21 ticks/10min). Ruled out: double free, probe hang, stack overflow, index back-derivation.
round 21 audit increments (source line-by-line re-review):
- `pbuf_alloced_custom`: PBUF_RAW's layer offset = 0 → payload = data exactly aligned ✓;
  `LWIP_PBUF_CUSTOM_DATA_INIT` is an empty macro → the preset custom_free_function is not overwritten ✓.
- `ethernet_input` success path (L248) **does not free the pbuf** (dispatch targets ip_input/etharp free it) →
  round 17's "free only on failure" fix direction was correct (the double free was indeed caused by round 11's dual branch).
- raw pcb contract: `recv` returning 1 = consumed (ping_recv self-frees + returns 1 ✓).
- Remaining suspects narrowed to: the DHCP slow-path head pbuf chain's free counting (PBUF_RAM head + REF chain),
  or serial/thread interaction related to desktop process load timing (needs runtime tracing, deferred).

**Decision**: production default = copy path (verified stable); zero-copy kept as experimental (`NET_ZEROCOPY=0`),
the suspect list above is the follow-up fix roadmap.

## Landed (observability + key fix history)

- Complete driver-layer counters: rx/tx packet count + bytes, loss, descriptor errors, ring full, IRQ count.
- lwIP thread outputs rate every 10 seconds (rx/s, tx/s) → long-term trend/leak/regression directly observable.
- **RTT percentile profiling**: every 5 minutes outputs ping RTT min/P50/P90/P99/max + loss count.
- **Zero-copy (explicit-index-passing version)**: pbuf_custom attached directly to DMA buffers, check-out/RecycleRx
  state machine + lent-out hole handling. Historical failures: ① index back-derived from address difference (independent page allocations non-contiguous → all wrong)
  ② kernel thread stack 16KB insufficient (lwIP deep paths + DHCP frame handling → stack overflow → thread silent death,
  once misdiagnosed as a ring race) —— both fixed, network thread uses `NewKernelThreadEx(..., 8)`.
- Stall self-diagnosis: 30 seconds without RX frames → dump the full ring state-machine picture.
