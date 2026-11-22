<!-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo -->
<!-- SPDX-License-Identifier: MIT -->

# xHCI enumeration depth (audit C12)

Audit item C12: xHCI enumeration depth — hubs, HID descriptors, EP0 max packet size — was a real
roadmap with no document behind it. This is that document.

## Implemented

- Controller init, MSI-X, command ring, event ring.
- EP0 control transfers on the synchronous path; the IRQ-context blocking problem is fixed.
- Port reset/enable and device address assignment (`PORTSC` W1C handling fixed).
- Single-device enumeration, verified on QEMU xHCI.

## Gaps, by priority

| # | Gap | Impact | Notes |
|---|---|---|---|
| 1 | Hub traversal: downstream ports of external hubs are not scanned | multi-level USB topologies on real hardware don't work | needs hub class requests and downstream port enumeration |
| 2 | HID Report Descriptor read | `hid.cpp` strips report IDs by heuristic (B7 already narrowed it using `bInterfaceProtocol`) | reading the descriptor allows exact parsing |
| 3 | EP0 max packet size negotiation: not adjusted per device descriptor | low/full-speed devices that use 8 or 64 instead of 512 on EP0 get transfer errors | update from `bMaxPacketSize0` after Get Device Descriptor |
| 4 | Synchronous/async chained-SG TRBs (B5 async path) | cross-page DMA errors | async goes through bounce buffers or chained TRBs |
| 5 | Bulk transfer retry and flow control, isochronous transfers | audio and mass-storage devices | feature work |

## Before starting

- A real-hardware xHCI controller. QEMU only covers the single-device path.
- The hub class and descriptor chapters of the USB 2.0/3.x specs.

## How to verify

Once hardware is available: enumeration and hot-plug regression across multi-level hubs with a mix of
keyboards and mice.
