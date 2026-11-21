#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT
"""make_modules_saf.py — pack all kernel .ko images into one SAF archive.

The kernel is booted by a single Limine module:
    module_path: boot():/boot/modules.saf
This script walks an input directory recursively, collects every *.ko, and
writes a flat SAF root folder whose children are file nodes. The kernel
parser (kernel/src/arch/x86_64/kmod/saf.cpp) only needs root + file nodes.

SAF layout (res/saf/SPECS.md):
  header = <QQ256sQ>  (magic, len, name, flags)
  folder = header + Q num_children + Q children[]
  file   = header + Q size + Q addr
All offsets are from the archive start. File data follows its node.
"""

import argparse
import os
import struct
import sys

MAGIC = 0x766863726C706D73
FLAG_FOLDER = 1
HDR = struct.Struct("<QQ256sQ")
FOLDER_PREFIX = 8  # num_children
FILE_PREFIX = 16   # size + addr


def node_name(name: str) -> bytes:
    raw = name.encode("utf-8")
    if len(raw) > 255:
        raise ValueError("SAF node name too long: %s" % name)
    return raw + b"\0" * (256 - len(raw))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input_dir")
    ap.add_argument("output")
    args = ap.parse_args()

    if not os.path.isdir(args.input_dir):
        print("input directory not found: %s" % args.input_dir, file=sys.stderr)
        return 2

    files = []
    for root, _dirs, names in os.walk(args.input_dir):
        for name in names:
            if name.endswith(".ko"):
                files.append(os.path.join(root, name))
    files.sort()
    if not files:
        print("no .ko files under %s" % args.input_dir, file=sys.stderr)
        return 2

    # Flat root: basenames must be unique (components + demos satisfy this).
    seen = set()
    entries = []
    for path in files:
        base = os.path.basename(path)
        if base in seen:
            print("duplicate module basename: %s" % base, file=sys.stderr)
            return 2
        seen.add(base)
        with open(path, "rb") as fh:
            blob = fh.read()
        entries.append((base, blob))

    n = len(entries)
    root_len = HDR.size + FOLDER_PREFIX + n * 8
    offsets = []
    cursor = root_len
    for _name, blob in entries:
        offsets.append(cursor)
        cursor += HDR.size + FILE_PREFIX + len(blob)

    out = bytearray(cursor)
    root = HDR.pack(MAGIC, root_len, node_name(""), FLAG_FOLDER)
    out[0:HDR.size] = root
    struct.pack_into("<Q", out, HDR.size, n)
    for i, off in enumerate(offsets):
        struct.pack_into("<Q", out, HDR.size + FOLDER_PREFIX + i * 8, off)

    for (name, blob), off in zip(entries, offsets):
        out[off:off + HDR.size] = HDR.pack(MAGIC, HDR.size + FILE_PREFIX,
                                           node_name(name), 0)
        struct.pack_into("<Q", out, off + HDR.size, len(blob))
        data_off = off + HDR.size + FILE_PREFIX
        struct.pack_into("<Q", out, off + HDR.size + 8, data_off)
        out[data_off:data_off + len(blob)] = blob

    with open(args.output, "wb") as fh:
        fh.write(out)
    print("SAF %s: %d modules, %d bytes" % (args.output, n, len(out)))
    for name, blob in entries:
        print("  %-48s %8d" % (name, len(blob)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
