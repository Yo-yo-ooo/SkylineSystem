# Network stack (e1000 82574L + lwIP porting)

> Goal deliverable document. Features, verification evidence, and known boundaries are all recorded truthfully.

## Components

| Component | Location | Description |
|---|---|---|
| e1000 driver | `kernel/src/drivers/net/e1000.cpp` + `kernel/include/drivers/net/e1000.h` | 82574L/82540EM compatible subset: probe/reset/EEPROM MAC/256 RX + 256 TX rings/IRQ+polling dual mode |
| lwIP glue | `kernel/src/drivers/net/e1000_lwip.cpp` | zero-copy experimental switch + copy production path, DHCP, ping profiling, multi-core TX flood |
| lwIP proper | `kernel/src/net/**` (vendored) | NO_SYS=1 polling mode |
| Port config | `kernel/include/lwip/lwipopts.h` + `cc.h` | see "porting fixes" below |
| Boot wiring | `init.cpp` NetStackInit (after scheduler installed) | e1000 probe at PCI enumeration |

## Verification evidence (all recorded in serial.log)

- **DHCP**: `[lwip] DHCP success: 10.0.2.15` + mask/gateway
- **ping full path**: `[lwip][ping] reply from 10.0.2.2` consecutive replies (hundreds cumulative)
- **Multi-core TX**: 3-core flood 2.2-2.7k frames/s, tx_full=0 loss=0 errors=0
- **RTT percentiles**: every 5 minutes min/P50/P90/P99/max + packet loss (dedicated ping profiling mode)
- **Long stability**: multi-segment 10-55min clean soak (0 assertions 0 exceptions), 2h closing soak see goal-status.md
- **Fault injection**: OOM injection into first 5 RX frames → loss counter + self-healing

## Performance profile snapshots (QEMU TCG, wall clock ~3x inflation —— see boundaries below)

| Round | Scenario | Result |
|---|---|---|
| round 89 | 30min soak (post-milestone) | tx=1,169,054 frames / 74.8MB, sustained 2815/s; loss=0 errors=0 full=0; ping 144 consecutive |
| round 95 | 5min collection | tx=199,935 frames / 12.8MB, climbs 2441→2510/s then stable; loss=0 errors=0 full=0 |

Multi-round trend: rate stable in the 2400-2800 frames/s band (TCG jitter range), zero loss / zero ring-full consistent across rounds.
Finer RTT min/P50/P90/P99 see the dedicated ping profiling mode (percentile output every 5 minutes).

## Porting fixes (all have bug→fix records)

1. lwipopts: software checksum (STM32 CHECKSUM_BY_HARDWARE removed), 16B alignment, pbuf pool,
   `LWIP_DHCP_DOES_ACD_CHECK=0` (slirp ARP proxy misjudges conflicts)
2. cc.h: `LWIP_PLATFORM_DIAG` varargs passed through directly (original implementation lost all arguments via assert's %s forwarding)
3. `sys_now`: RTC date encoding → PIT monotonic milliseconds (cross-day wraparound would break timeout math)
4. Network thread stack 32KB (`NewKernelThreadEx`): default 16KB overflows under lwIP deep paths + DHCP handling
   → thread dies silently (once misdiagnosed as various races)
5. In-driver DMA buffer: static BSS GetPhysics unresolvable → VMM::Alloc page + in-page offset
6. Zero-copy history: index back-derivation from address difference (independent pages non-contiguous) + pbuf double free + PIT::Sleep hang during probe
   —— all fixed; remaining suspects see docs/dpdk-lite.md

## Known boundaries (as-is)

- **RX receive-side checksum verification disabled** (lwipopts.h comment): transmit-side checksums fully on, receive-side verification path
  drops slirp's legal frames, pending verification against the lwIP version
- **Zero-copy = experimental** (NET_ZEROCOPY=0): production path is copy-based, stably verified
- **IRQ mode now enabled** (P3-80 fix: e1000.cpp's INTx interrupt line + IOAPIC remapping
  wired up, IRQ drives packet reception; the old "not enabled / polling mode" account is outdated); MSI-X multi-queue =
  blocked on 82574 datasheet verification
- **No IPv6/TCP real-traffic test**: default port config compiles in TCP/IPv6, not verified with real traffic
- All rate/latency numbers = QEMU TCG guest virtual time (wall clock ~3x inflation, see round 16 records)

## Related docs

- DPDK-lite design and zero-copy audit: `docs/dpdk-lite.md`
- Goal ledger and remaining debt: `docs/goal-status.md`
- Regression gating: `tests/net_smoke.ps1` (QEMU smoke)
