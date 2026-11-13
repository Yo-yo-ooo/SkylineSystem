#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT
"""build_os_pkg.py — 生成 Setup 用的 SKYOS1 整个操作系统安装包。

产物: programs/bin/os.pkg（根 Makefile 的 `make os-pkg` 再 e2cp 到 disk.img:/os.pkg）

容器格式（全部小端；自包含、可审计）::

    header:
      char magic[8]      "SKYOS1\\0\\0"
      u32  version       1
      u32  entry_count
      u32  flags         bit0 = 含 data/doom.wad；其余保留 0
      u32  reserved      0
    每条 entry:
      u16  path_len      安装路径（相对 /mp/system/，"/" 分隔，不含 ".."）
      u16  reserved      0
      u32  method        0 = raw, 1 = LZ4 block
      u32  orig_size     解压后字节数
      u32  nblocks       块数
      char path[path_len]
    随后 nblocks 个 block:
      u32  orig_len      本块原始字节数（<= 64 KiB；最后一块可更小）
      u32  comp_len      本块压缩字节数（method=1）或 orig_len（method=0）
      u8   data[comp_len]
    method=0: nblocks 必须为 1，comp_len == orig_len。
    method=1: 每块是独立的 standard LZ4 block（无 frame 头、无校验和）；
              安装器按 64 KiB 块解压后 fwrite，不需要把整包/整文件读进内存。

压缩实现：Python 侧自己做合法 LZ4 block 贪心编码（4 字节 hash 表 + 匹配扩展，
遵守 "最后 5 字节必须是 literals / 最后一段 match 不得晚于 n-12 开始" 规则）。
若某文件压缩后不比原文小，则整文件回退为 method=0（raw），避免安装器为已经
不可压缩的数据白解压。

自检：脚本内置独立 LZ4 block 解码器，生成后把每个 entry 解码并与源文件逐字节
比较（roundtrip），失败即退出非 0。这样安装器 C 解码器与 Python 编码器的
格式一致性有构建期证据。

用法::

    python3 res/scripts/build_os_pkg.py                # 默认: 不含 doom.wad
    python3 res/scripts/build_os_pkg.py --with-doom-wad
    python3 res/scripts/build_os_pkg.py --root /mnt/c/ZSY/SkylineSystem --out /tmp/os.pkg
"""

import argparse
import os
import struct
import sys
import time

MAGIC = b"SKYOS1\x00\x00"
VERSION = 1
BLOCK = 64 * 1024
FLAG_HAS_WAD = 1 << 0

# 固定 entry: (安装路径, 仓库内相对路径)。存在才收，缺失跳过（并打印）。
FIXED_ENTRIES = [
    ("boot/kernel", "kernel/bin-x86_64/kernel"),
    ("lib/libsky.so", "lib/bin/libsky.so"),
    ("config/desktop.lua", "programs/desktop/desktop.lua"),
    ("boot/limine/limine.conf", "res/limine.conf"),
    ("boot/limine/limine-bios.sys", "limine-binary/limine-bios.sys"),
    ("boot/limine/BOOTX64.EFI", "limine-binary/BOOTX64.EFI"),
]

PROGRAM_ELFS = ["desktop", "notepad", "settings", "installer", "doom",
                "explorer", "hw", "hw2"]

MODULE_GLOB_DIR = "kernel/bin-x86_64/modules"
DOOM_WAD = "programs/PureDOOM/doom.wad"


# ---------------------------------------------------------------------------
# LZ4 block compressor (greedy, hash table on 4-byte groups)
# ---------------------------------------------------------------------------
def _emit_len(out: bytearray, value: int) -> None:
    """LZ4 长度扩展字节（15 已含在高 nibble 里，这里只写 remainder）。"""
    while value >= 255:
        out.append(255)
        value -= 255
    out.append(value)


def lz4_compress_block(src: bytes) -> bytes:
    """编码一个独立的 LZ4 block（0 <= len(src) <= 64 KiB 时最典型）。"""
    n = len(src)
    out = bytearray()
    if n == 0:
        # 空块：单个 literal-only sequence，literal 长度 0。
        out.append(0)
        return bytes(out)

    # "The last match must start at least 12 bytes before the end of block.
    #  The last 5 bytes of input are always literals."  —— LZ4 block 规范。
    last_match_start = n - 12
    last_match_end = n - 5
    table = {}  # 4-byte group -> 最近一次出现位置
    i = 0
    anchor = 0
    while i <= last_match_start:
        key = src[i:i + 4]
        hit = table.get(key)
        table[key] = i
        if hit is not None and (i - hit) <= 0xFFFF and hit < i:
            max_len = last_match_end - i
            if max_len >= 4:
                mlen = 4
                while mlen < max_len and src[hit + mlen] == src[i + mlen]:
                    mlen += 1
                if mlen >= 4:
                    lit_len = i - anchor
                    token_lit = 15 if lit_len >= 15 else lit_len
                    token_ml = 15 if (mlen - 4) >= 15 else (mlen - 4)
                    out.append((token_lit << 4) | token_ml)
                    if lit_len >= 15:
                        _emit_len(out, lit_len - 15)
                    out += src[anchor:i]
                    out.append((i - hit) & 0xFF)
                    out.append(((i - hit) >> 8) & 0xFF)
                    if (mlen - 4) >= 15:
                        _emit_len(out, (mlen - 4) - 15)
                    i += mlen
                    anchor = i
                    continue
        i += 1

    # 结尾 literals（必然包含最后 5 字节；若前面没有 match 则整块都是 literals）。
    lit_len = n - anchor
    token_lit = 15 if lit_len >= 15 else lit_len
    out.append(token_lit << 4)
    if lit_len >= 15:
        _emit_len(out, lit_len - 15)
    out += src[anchor:n]
    return bytes(out)


def lz4_decompress_block(src: bytes, expected: int) -> bytes:
    """独立的标准 LZ4 block 解码器（仅用于构建期 roundtrip 自检）。"""
    out = bytearray()
    i = 0
    n = len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                if i >= n:
                    raise ValueError("truncated literal length")
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        if i + lit > n:
            raise ValueError("literal overruns block")
        out += src[i:i + lit]
        i += lit
        if i == n:
            break  # 最后一个 sequence 只有 literals
        if i + 2 > n:
            raise ValueError("truncated match offset")
        off = src[i] | (src[i + 1] << 8)
        i += 2
        if off == 0 or off > len(out):
            raise ValueError("invalid match offset %d (out=%d)" % (off, len(out)))
        mlen = (token & 0x0F) + 4
        if (token & 0x0F) == 15:
            while True:
                if i >= n:
                    raise ValueError("truncated match length")
                b = src[i]
                i += 1
                mlen += b
                if b != 255:
                    break
        start = len(out) - off
        for k in range(mlen):
            out.append(out[start + k])
    if len(out) != expected:
        raise ValueError("decoded %d bytes, expected %d" % (len(out), expected))
    return bytes(out)


# ---------------------------------------------------------------------------
# 收集源文件
# ---------------------------------------------------------------------------
def collect_entries(root: str, with_wad: bool):
    """返回 [(install_path, src_abs_path, size)]，按 install_path 排序。"""
    entries = []

    def add(install_path: str, rel: str):
        if install_path.startswith("/") or ".." in install_path.split("/"):
            raise SystemExit("unsafe install path: %r" % install_path)
        src = os.path.join(root, rel)
        if not os.path.isfile(src):
            print("[os-pkg] skip missing: %s" % rel)
            return
        entries.append((install_path, src, os.path.getsize(src)))

    for install_path, rel in FIXED_ENTRIES:
        add(install_path, rel)

    for name in PROGRAM_ELFS:
        add("programs/%s.elf" % name, "programs/bin/%s.elf" % name)

    mod_dir = os.path.join(root, MODULE_GLOB_DIR)
    if os.path.isdir(mod_dir):
        for fn in sorted(os.listdir(mod_dir)):
            if fn.endswith(".ko"):
                add("boot/modules/%s" % fn, os.path.join(MODULE_GLOB_DIR, fn))

    if with_wad:
        add("data/doom.wad", DOOM_WAD)
    elif os.path.isfile(os.path.join(root, DOOM_WAD)):
        print("[os-pkg] note: %s present but excluded "
              "(use --with-doom-wad to include; keeps QEMU install fast)" % DOOM_WAD)

    entries.sort(key=lambda e: e[0])
    return entries


# ---------------------------------------------------------------------------
# 打包（每个文件独立选择 raw / lz4）
# ---------------------------------------------------------------------------
def encode_file(data: bytes):
    """返回 (method, blocks)；blocks = [(orig_len, comp_bytes)]。"""
    if not data:
        return 1, [(0, lz4_compress_block(b""))]
    blocks = []
    total_comp = 0
    for off in range(0, len(data), BLOCK):
        chunk = data[off:off + BLOCK]
        comp = lz4_compress_block(chunk)
        total_comp += len(comp)
        blocks.append((len(chunk), comp))
    if total_comp >= len(data):
        return 0, [(len(data), data)]
    return 1, blocks


def build_package(entries, out_path: str, with_wad: bool):
    t0 = time.time()
    header_size = 8 + 4 * 4
    chunks = []
    index = []
    raw_total = 0
    n_blocks_total = 0
    lz4_files = 0
    raw_files = 0
    for install_path, src, size in entries:
        with open(src, "rb") as fp:
            data = fp.read()
        if len(data) != size:
            raise SystemExit("size changed while reading: %s" % src)
        method, blocks = encode_file(data)
        enc_path = install_path.encode("utf-8")
        if len(enc_path) > 0xFFFF:
            raise SystemExit("install path too long: %s" % install_path)
        entry = bytearray()
        entry += struct.pack("<HHIII", len(enc_path), 0, method, len(data),
                             len(blocks))
        entry += enc_path
        for orig_len, comp in blocks:
            entry += struct.pack("<II", orig_len, len(comp))
            entry += comp
        chunks.append(bytes(entry))
        index.append((install_path, len(data), len(entry), method, len(blocks)))
        raw_total += len(data)
        n_blocks_total += len(blocks)
        if method == 1:
            lz4_files += 1
        else:
            raw_files += 1

    flags = FLAG_HAS_WAD if (with_wad and any(p == "data/doom.wad" for p, _, _ in entries)) else 0
    header = MAGIC + struct.pack("<IIII", VERSION, len(entries), flags, 0)
    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as fp:
        fp.write(header)
        for c in chunks:
            fp.write(c)
    pkg_size = os.path.getsize(out_path)

    print("[os-pkg] entries=%d files (lz4=%d, raw=%d), blocks=%d" %
          (len(entries), lz4_files, raw_files, n_blocks_total))
    print("[os-pkg] raw=%d B payload=%d B (ratio %.3f)" %
          (raw_total, pkg_size - header_size,
           (pkg_size - header_size) / raw_total if raw_total else 1.0))
    for install_path, size, enc_size, method, nblk in index:
        print("[os-pkg]   %-36s orig=%-9d stored=%-9d method=%s blocks=%d" %
              (install_path, size, enc_size, "lz4" if method else "raw", nblk))
    print("[os-pkg] wrote %s (%d B, header %d B)" %
          (out_path, pkg_size, header_size))
    print("[os-pkg] build time: %.2f s" % (time.time() - t0))
    return pkg_size, raw_total, index


# ---------------------------------------------------------------------------
# 构建期 roundtrip 自检：用独立解码器把生成包完整解开，与源文件逐字节比较
# ---------------------------------------------------------------------------
def self_test(pkg_path: str, entries):
    t0 = time.time()
    with open(pkg_path, "rb") as fp:
        blob = fp.read()
    if blob[:8] != MAGIC:
        raise SystemExit("[os-pkg] SELF-TEST FAIL: bad magic")
    version, entry_count, _flags, _reserved = struct.unpack_from("<IIII", blob, 8)
    if version != VERSION:
        raise SystemExit("[os-pkg] SELF-TEST FAIL: version %d" % version)
    if entry_count != len(entries):
        raise SystemExit("[os-pkg] SELF-TEST FAIL: entry_count %d != %d" %
                         (entry_count, len(entries)))
    src_by_path = {p: s for p, s, _ in entries}
    off = 24
    checked = 0
    decoded_bytes = 0
    for e in range(entry_count):
        path_len, _r, method, orig_size, nblocks = struct.unpack_from(
            "<HHIII", blob, off)
        off += 16  # u16 path_len + u16 reserved + u32 method + u32 orig + u32 nblocks
        path = blob[off:off + path_len].decode("utf-8")
        off += path_len
        data = bytearray()
        for _b in range(nblocks):
            orig_len, comp_len = struct.unpack_from("<II", blob, off)
            off += 8
            comp = blob[off:off + comp_len]
            off += comp_len
            if method == 0:
                if comp_len != orig_len or nblocks != 1:
                    raise SystemExit("[os-pkg] SELF-TEST FAIL: bad raw block %s" % path)
                data += comp
            else:
                try:
                    data += lz4_decompress_block(comp, orig_len)
                except ValueError as exc:
                    raise SystemExit("[os-pkg] SELF-TEST FAIL: %s block %d: %s" %
                                     (path, _b, exc))
        if len(data) != orig_size:
            raise SystemExit("[os-pkg] SELF-TEST FAIL: %s size %d != %d" %
                             (path, len(data), orig_size))
        with open(src_by_path[path], "rb") as fp:
            if fp.read() != bytes(data):
                raise SystemExit("[os-pkg] SELF-TEST FAIL: %s content mismatch" % path)
        checked += 1
        decoded_bytes += len(data)
    if off != len(blob):
        raise SystemExit("[os-pkg] SELF-TEST FAIL: %d trailing bytes" %
                         (len(blob) - off))
    print("[os-pkg] LZ4 roundtrip self-test OK: %d entries, %d B decoded (%.2f s)" %
          (checked, decoded_bytes, time.time() - t0))


# ---------------------------------------------------------------------------
def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="build the SKYOS1 OS install package")
    here = os.path.dirname(os.path.abspath(__file__))
    default_root = os.path.abspath(os.path.join(here, "..", ".."))
    ap.add_argument("--root", default=default_root, help="repository root")
    ap.add_argument("--out", default=None,
                    help="output path (default: <root>/programs/bin/os.pkg)")
    ap.add_argument("--with-doom-wad", action="store_true",
                    help="include programs/PureDOOM/doom.wad (large, optional)")
    ap.add_argument("--no-self-test", action="store_true",
                    help="skip the decode/roundtrip verification (not recommended)")
    args = ap.parse_args(argv)

    root = os.path.abspath(args.root)
    out_path = args.out or os.path.join(root, "programs", "bin", "os.pkg")
    entries = collect_entries(root, args.with_doom_wad)
    if not entries:
        print("[os-pkg] ERROR: no source files found under %s" % root,
              file=sys.stderr)
        return 1
    pkg_size, raw_total, _index = build_package(entries, out_path,
                                                args.with_doom_wad)
    if not args.no_self_test:
        self_test(out_path, entries)
    print("[os-pkg] OK: %s (%d B; raw %d B)" % (out_path, pkg_size, raw_total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
