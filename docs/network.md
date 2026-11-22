<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# Network stack: e1000 82574L and the lwIP port

## Components

| Component | Location | What it is |
|---|---|---|
| e1000 driver | `kernel/modules/common/drivers/net/e1000.cpp` + `kernel/include/drivers/net/e1000.h` | 82574L/82540EM compatible subset: probe, reset, EEPROM MAC, 256-entry RX and TX rings, IRQ plus polling |
| lwIP glue | `kernel/modules/common/drivers/net/e1000_lwip.cpp` | zero-copy experimental switch, copy-based production path, DHCP, ping profiling, multi-core TX flood |
| lwIP itself | `kernel/modules/common/net/**` (vendored) | `NO_SYS=1`, polling mode |
| Port config | `kernel/include/lwip/lwipopts.h` + `cc.h` | see the porting fixes below |
| Boot wiring | `init.cpp` `NetStackInit` (runs after the scheduler is installed) | e1000 probes during PCI enumeration |

## Evidence

All of this shows up in `serial.log`:

- DHCP: `[lwip] DHCP success: 10.0.2.15`, plus mask and gateway.
- Ping, full path: `[lwip][ping] reply from 10.0.2.2`, consecutive replies, hundreds cumulative.
- Multi-core TX: three cores flooding 2.2-2.7k frames/s, `tx_full=0 loss=0 errors=0`.
- RTT percentiles every 5 minutes: min/P50/P90/P99/max plus loss count, in the dedicated ping
  profiling mode.
- Long runs: several clean 10-55 minute soaks with zero assertions and zero exceptions. The 2-hour
  soak is recorded in goal-status.md.
- Fault injection: OOM injected into the first 5 RX frames produces a loss count and then self-heals.

## Throughput snapshots

QEMU TCG, guest virtual time; wall clock runs roughly 3x faster.

| Run | Scenario | Result |
|---|---|---|
| 1 | 30min soak | tx=1,169,054 frames / 74.8MB, sustained 2815/s; loss=0 errors=0 full=0; 144 consecutive pings |
| 2 | 5min collection | tx=199,935 frames / 12.8MB, climbing 2441→2510/s then flat; loss=0 errors=0 full=0 |

Across runs the rate sits in the 2400-2800 frames/s band, which is TCG jitter, with no loss and no
ring-full anywhere. For finer RTT numbers use the ping profiling mode, which prints percentiles
every 5 minutes.

## Porting fixes

1. `lwipopts`: software checksums (the STM32 `CHECKSUM_BY_HARDWARE` define removed), 16B alignment, pbuf
   pool size, `LWIP_DHCP_DOES_ACD_CHECK=0` (slirp's ARP proxy otherwise looks like an address conflict).
2. `cc.h`: `LWIP_PLATFORM_DIAG` passes varargs straight through. The original implementation forwarded
   everything through an assert's `%s` and lost every argument.
3. `sys_now`: switched from RTC date encoding to PIT monotonic milliseconds. Crossing midnight broke
   the timeout arithmetic.
4. Network thread stack of 32KB through `NewKernelThreadEx`. The 16KB default overflowed on lwIP's
   deep paths plus DHCP handling, and the thread died silently — for a while that was misdiagnosed as
   several different races.
5. In-driver DMA buffer: a static BSS buffer can't resolve through `GetPhysics`, so it became a
   `VMM::Alloc` page plus an in-page offset.
6. Zero-copy history: index back-derived from address differences (independent pages aren't contiguous),
   a pbuf double free, and `PIT::Sleep` hanging during probe. All fixed; what's left is in
   [dpdk-lite.md](dpdk-lite.md).

## Boundaries

- RX checksum verification is off (see the comment in `lwipopts.h`). Transmit checksums are fully on;
  the receive-side verification path drops slirp's otherwise legal frames, and it needs checking
  against this lwIP version before turning on.
- Zero-copy is experimental, `NET_ZEROCOPY=0`. Production uses the copy path, which is verified stable.
- IRQ mode is enabled now: `e1000.cpp` wires the INTx line through IOAPIC remapping and IRQs drive
  packet reception. The older "not enabled, polling only" description is out of date. MSI-X multi-queue
  is blocked on verifying the 82574 datasheet.
- No real-traffic test for IPv6 or TCP yet. The default port config compiles both in.
- Every rate and latency number here is QEMU TCG guest virtual time, where wall clock runs ~3x faster.

## Related

- DPDK-lite design and zero-copy audit: [dpdk-lite.md](dpdk-lite.md)
- Goal ledger and remaining debt: [goal-status.md](goal-status.md)
- Regression gate: `tests/net_smoke.ps1` (QEMU smoke)
