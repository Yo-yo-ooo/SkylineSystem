# xHCI Enumeration Depth Roadmap (C12)

> Audit C12: the xHCI enumeration depth (hub/HID descriptors/EP0 mps) is a feature roadmap
> that had no documentation at all. This file archives it (round 13).

## Current State (Implemented)

- Controller initialization, MSI-X, command ring, event ring
- EP0 control transfers (synchronous path; the P1-32 IRQ-context blocking issue is fixed)
- Port reset/enable, device address assignment (PORTSC W1C fixed in P1-24)
- Single-device enumeration (verified on QEMU xHCI)

## Gaps (by Priority)

| # | Gap | Impact | Notes |
|---|---|---|---|
| 1 | **hub traversal** (downstream ports of external hubs are not scanned) | multi-level USB topologies on real hardware are unusable | needs hub class requests + downstream port enumeration |
| 2 | **HID Report Descriptor read** | hid.cpp's report-ID stripping can only rely on heuristics (B7 already narrowed it by bInterfaceProtocol) | reading the descriptor enables exact parsing |
| 3 | **EP0 mps negotiation** (max packet size is not adjusted per the device descriptor) | transfer errors when low/full-speed devices use 8/64 instead of 512 on EP0 | update per bMaxPacketSize0 after Get Device Descriptor |
| 4 | sync/async chained-SG TRBs (B5 async path) | cross-page DMA errors | async goes through bounce buffers or chained TRBs |
| 5 | bulk transfer retry/flow control, isochronous transfers | audio / mass-storage devices | feature extension |

## Implementation Prerequisites

- A real-hardware xHCI controller (QEMU only covers the single-device path)
- The hub class and descriptor chapters of the USB 2.0/3.x specifications

## Verification Plan

Once real hardware is available: enumeration and hot-plug regression over multi-level hub + keyboard/mouse mixed topologies.
