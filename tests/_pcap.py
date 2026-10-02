#!/usr/bin/env python3
import struct, sys
path = "/mnt/c/ZSY/SkylineSystem/net.pcap"
data = open(path, "rb").read()
off = 24
n = 0
while off + 16 <= len(data) and n < 24:
    ts_sec, ts_usec, incl, orig = struct.unpack("<IIII", data[off:off+16])
    off += 16
    if off + incl > len(data):
        break
    frame = data[off:off+incl]
    off += incl
    if len(frame) >= 14:
        dst = frame[0:6].hex(":")
        src = frame[6:12].hex(":")
        et = (frame[12] << 8) | frame[13]
        kind = {0x0800: "IPv4", 0x0806: "ARP", 0x8100: "802.1Q"}.get(et, hex(et))
        extra = ""
        if et == 0x0800 and len(frame) >= 34:
            proto = frame[23]
            extra = {1: "ICMP", 17: "UDP", 6: "TCP"}.get(proto, f"IPproto={proto}")
        print(f"n={n} {incl}B {src} -> {dst} {kind} {extra}")
    n += 1
print("total frames:", n)
