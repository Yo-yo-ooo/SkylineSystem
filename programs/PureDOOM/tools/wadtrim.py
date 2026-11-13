#!/usr/bin/env python3
"""wadtrim.py -- host-side WAD trimmer for SkylineSystem's DOOM_TINY build.

The Skyline DOOM port has no audio driver (DOOM_FLAG_HIDE_SOUND/MUSIC and no
output device), so every sound/music lump in the IWAD is dead weight. This tool
parses the IWAD directory, drops the audio lumps, and rewrites a *legal* WAD
(header + lumps + directory) that the unmodified engine can load.

Default trim (what the tiny build ships):
  * drop music           : lumps whose 8-char name starts with "D_"
  * drop DMX sound FX    : lumps whose name starts with "DS"
  * drop PC-speaker SFX  : lumps whose name starts with "DP"
Everything else (PLAYPAL, COLORMAP, textures, sprites, maps, DEMO*) is copied
byte-for-byte in the original order.

Engine coupling (why dropping *all* audio is safe here):
  PureDOOM.h has a DOOM_TINY audio stub: I_InitSound() skips the sfx pre-cache,
  S_StartSoundAtVolume()/S_ChangeMusic()/S_UpdateSounds() return immediately,
  so no DS*/D_* lump is ever looked up. In a normal (DOOM_TINY=0) build the
  engine pre-caches every sfx (getsfx falls back to "dspistol") and looks up
  "d_<song>" per level, so a trimmed IWAD must NOT be used there; use the
  untouched IWAD (or --keep-audio) for that configuration.

--adpcm (audit / potential future use, NOT the default payload):
  keeps the DS* lumps but transcodes their DMX 8-bit PCM payloads to standard
  IMA-ADPCM (4-bit nibbles, 256-byte blocks: 4-byte header + 504 samples). That
  is ~4:1 versus 16-bit PCM (the format IMA-ADPCM is quoted against) and ~2:1
  versus the original 8-bit DMX data. The encoded lump self-describes:
      "IMA1" | u32 nsamples | u32 encoded_len | IMA-ADPCM blocks
  The engine never loads these lumps while audio is stripped, so this mode is
  for size accounting / round-trip proof (the ADPCM-XQ idea: codec-level 4:1
  before any container compression).
  NOTE: for an actually deployable image use the default trim: all audio gone.

Usage:
  python3 tools/wadtrim.py INPUT.wad OUTPUT.wad [--adpcm] [--stats] [--json]
                                           [--verify] [--keep-audio]
  python3 tools/wadtrim.py --self-test

Exit codes: 0 ok, 1 error (bad WAD / verification failure).
"""
import argparse
import json
import os
import struct
import sys
import tempfile

WAD_MAGIC = (b"IWAD", b"PWAD")
MUSIC_PREFIX = "D_"
SFX_PREFIX = "DS"
PCSPK_PREFIX = "DP"
REQUIRED_LUMPS = ("PLAYPAL", "COLORMAP")
REQUIRED_MAP_MARKERS = ("E1M1", "MAP01")


class WadError(Exception):
    pass


class Lump:
    __slots__ = ("name", "data", "index")

    def __init__(self, name, data, index):
        self.name = name
        self.data = data
        self.index = index


def read_wad(path):
    data = open(path, "rb").read()
    if len(data) < 12:
        raise WadError("file too small for a WAD header")
    magic = data[0:4]
    if magic not in WAD_MAGIC:
        raise WadError("bad WAD magic %r (need IWAD/PWAD)" % (magic,))
    numlumps, infotableofs = struct.unpack_from("<ii", data, 4)
    if numlumps <= 0 or numlumps > 100000:
        raise WadError("implausible lump count %d" % numlumps)
    if infotableofs < 12 or infotableofs + numlumps * 16 > len(data):
        raise WadError("directory out of range")
    lumps = []
    for i in range(numlumps):
        off = infotableofs + i * 16
        filepos, size = struct.unpack_from("<ii", data, off)
        rawname = data[off + 8:off + 16]
        name = rawname.split(b"\x00", 1)[0].decode("ascii", "replace")
        if size < 0 or filepos < 0 or filepos + size > len(data):
            raise WadError("lump %d (%s) data out of range" % (i, name))
        lumps.append(Lump(name, data[filepos:filepos + size], i))
    return magic, lumps


def write_wad(path, magic, lumps):
    header = bytearray(magic)
    header += struct.pack("<ii", len(lumps), 0)  # table offset patched below
    body = bytearray()
    entries = []
    for lump in lumps:
        name = lump.name.encode("ascii", "replace")[:8]
        name = name + b"\x00" * (8 - len(name))
        entries.append(struct.pack("<ii", len(header) + len(body), len(lump.data))
                       + name)
        body += lump.data
    table_ofs = len(header) + len(body)
    struct.pack_into("<ii", header, 4, len(lumps), table_ofs)
    with open(path, "wb") as f:
        f.write(header)
        f.write(body)
        f.write(b"".join(entries))
    return table_ofs


# --------------------------------------------------------------------------
# IMA-ADPCM encoder (standard 4-bit IMA, the ADPCM-XQ target codec)
# --------------------------------------------------------------------------
IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41,
    45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190,
    209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500,
    20350, 22385, 24623, 27086, 29794, 32767,
]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

ADPCM_BLOCK_BYTES = 256                      # 4-byte header + 252 nibble bytes
ADPCM_SAMPLES_PER_BLOCK = 1 + (ADPCM_BLOCK_BYTES - 4) * 2   # 505


def ima_encode_s16(samples):
    """Encode an iterable of int16 samples; returns (payload, overhead_bytes)."""
    out = bytearray()
    predictor = 0
    index = 0
    n = len(samples)
    pos = 0
    while pos < n:
        if pos == 0:
            predictor = samples[0]
            index = 0
        out += struct.pack("<hBB", predictor, index, 0)
        nibble_pairs = []
        pos += 1
        while pos < n and len(nibble_pairs) < (ADPCM_BLOCK_BYTES - 4) * 2:
            code = 0
            step = IMA_STEP[index]
            diff = samples[pos] - predictor
            if diff < 0:
                code = 8
                diff = -diff
            delta = 0
            if diff >= step:
                delta |= 4
                diff -= step
            if diff >= (step >> 1):
                delta |= 2
                diff -= step >> 1
            if diff >= (step >> 2):
                delta |= 1
            code |= delta
            step = IMA_STEP[index]
            diffq = step >> 3
            if code & 4:
                diffq += step
            if code & 2:
                diffq += step >> 1
            if code & 1:
                diffq += step >> 2
            if code & 8:
                predictor -= diffq
            else:
                predictor += diffq
            if predictor > 32767:
                predictor = 32767
            elif predictor < -32768:
                predictor = -32768
            index += IMA_INDEX[code]
            if index < 0:
                index = 0
            elif index > 88:
                index = 88
            nibble_pairs.append(code & 0xF)
            pos += 1
        if len(nibble_pairs) & 1:
            nibble_pairs.append(0)          # pad a lone nibble with 0
        for i in range(0, len(nibble_pairs), 2):
            out.append((nibble_pairs[i] & 0xF) | ((nibble_pairs[i + 1] & 0xF) << 4))
    return bytes(out), (len(out) - (n * 0.5) if n else 0)


def dmx_to_adpcm_lump(name, data, stats):
    """Convert a DMX sound lump (8-byte header + u8 PCM) to an IMA1 lump.

    Falls back to keeping the original bytes (marked in stats) if the lump is
    not a DMX format-3 sound; the tool never produces garbage silently.
    """
    if len(data) < 8:
        stats["adpcm_skipped"] += 1
        return data
    fmt, rate, nsamples = struct.unpack_from("<HHI", data, 0)
    if fmt != 3 or nsamples == 0 or 8 + nsamples > len(data):
        stats["adpcm_skipped"] += 1
        return data
    pcm = data[8:8 + nsamples]
    samples = [(b - 128) << 8 for b in pcm]     # u8 -> signed 16-bit
    payload, _ = ima_encode_s16(samples)
    encoded = b"IMA1" + struct.pack("<II", nsamples, len(payload)) + payload
    stats["adpcm_lumps"] += 1
    stats["adpcm_in_bytes"] += len(data)
    stats["adpcm_out_bytes"] += len(encoded)
    stats["adpcm_pcm16_bytes"] += nsamples * 2
    return encoded


# --------------------------------------------------------------------------

def classify(name, drop_banks):
    if name.startswith(MUSIC_PREFIX):
        return "music"
    if name.startswith(SFX_PREFIX):
        return "sfx"
    if name.startswith(PCSPK_PREFIX):
        return "pcspk"
    if drop_banks and name in ("GENMIDI", "DMXGUS", "DMXGUSC"):
        return "music-bank"
    return None


def trim(magic, lumps, adpcm, keep_audio, drop_banks):
    kept = []
    dropped = {"music": 0, "sfx": 0, "pcspk": 0, "music-bank": 0}
    dropped_bytes = {"music": 0, "sfx": 0, "pcspk": 0, "music-bank": 0}
    adpcm_stats = {
        "adpcm_lumps": 0, "adpcm_in_bytes": 0, "adpcm_out_bytes": 0,
        "adpcm_pcm16_bytes": 0, "adpcm_skipped": 0,
    }
    for lump in lumps:
        kind = classify(lump.name, drop_banks)
        if keep_audio or kind is None:
            kept.append(lump)
            continue
        if adpcm and kind == "sfx":
            kept.append(Lump(lump.name, dmx_to_adpcm_lump(lump.name, lump.data,
                                                         adpcm_stats), lump.index))
            continue
        dropped[kind] += 1
        dropped_bytes[kind] += 8 + len(lump.data)  # lump + 16B dir share ~8B
    return kept, dropped, dropped_bytes, adpcm_stats


def verify_output(in_magic, in_lumps, out_path, kept, adpcm):
    """Re-read OUTPUT and prove it is a legal WAD with exactly the kept lumps."""
    magic, lumps = read_wad(out_path)
    if magic != in_magic:
        raise WadError("output magic changed")
    if len(lumps) != len(kept):
        raise WadError("output lump count %d != expected %d"
                       % (len(lumps), len(kept)))
    for got, want in zip(lumps, kept):
        if got.name != want.name:
            raise WadError("lump order/name mismatch: %r != %r"
                           % (got.name, want.name))
        if got.data != want.data:
            raise WadError("lump %r payload mismatch after rewrite" % got.name)
    names = [l.name for l in lumps]
    for req in REQUIRED_LUMPS:
        if req not in names:
            raise WadError("required lump %r missing after trim" % req)
    if not any(m in names for m in REQUIRED_MAP_MARKERS):
        raise WadError("no map marker (%s) after trim"
                       % "/".join(REQUIRED_MAP_MARKERS))
    if adpcm:
        for l in lumps:
            if l.name.startswith(SFX_PREFIX):
                if not l.data.startswith(b"IMA1"):
                    raise WadError("sfx lump %r was not ADPCM-tagged" % l.name)
                nsamples, enc_len = struct.unpack_from("<II", l.data, 4)
                if enc_len != len(l.data) - 12:
                    raise WadError("sfx lump %r IMA1 length mismatch" % l.name)
    return lumps


def self_test():
    """Build a synthetic WAD in a temp dir and round-trip it through trim."""
    tmp = tempfile.mkdtemp(prefix="wadtrim-selftest-")
    src = os.path.join(tmp, "self.wad")
    out = os.path.join(tmp, "self_trim.wad")
    lumps = [
        Lump("PLAYPAL", bytes(range(256)) * 3, 0),
        Lump("COLORMAP", bytes(range(256)) * 34, 1),
        Lump("E1M1", b"mapmarker" + bytes(64), 2),
        Lump("D_E1M1", b"music" * 1000, 3),
        Lump("D_INTER", b"music" * 500, 4),
        Lump("DSBAREXP", struct.pack("<HHI", 3, 11025, 200) + bytes(200), 5),
        Lump("DSPISTOL", struct.pack("<HHI", 3, 11025, 100) + bytes([0, 255]) * 50, 6),
        Lump("DPHOOF", b"pcspk" * 300, 7),
        Lump("DEMO1", bytes(500), 8),
    ]
    write_wad(src, b"IWAD", lumps)
    magic, in_lumps = read_wad(src)
    kept, dropped, _db, adpcm_stats = trim(magic, in_lumps, False, False, False)
    write_wad(out, magic, kept)
    verify_output(magic, in_lumps, out, kept, False)
    if dropped["music"] != 2 or dropped["sfx"] != 2 or dropped["pcspk"] != 1:
        raise WadError("self-test drop histogram wrong: %r" % (dropped,))
    # round-trip the ADPCM variant too
    kept2, _d2, _db2, st2 = trim(magic, in_lumps, True, False, False)
    write_wad(out, magic, kept2)
    verify_output(magic, in_lumps, out, kept2, True)
    if st2["adpcm_lumps"] != 2 or st2["adpcm_out_bytes"] >= st2["adpcm_in_bytes"]:
        raise WadError("self-test adpcm stats wrong: %r" % (st2,))
    print("[wadtrim] self-test OK (drop + adpcm round-trip verified in %s)" % tmp)
    return 0


def fmt_bytes(n):
    if n >= 1024 * 1024:
        return "%.2f MiB" % (n / (1024.0 * 1024.0))
    if n >= 1024:
        return "%.1f KiB" % (n / 1024.0)
    return "%d B" % n


def main():
    ap = argparse.ArgumentParser(
        description="Trim audio lumps out of a DOOM IWAD for DOOM_TINY "
                    "(and optionally report IMA-ADPCM 4:1 stats).")
    ap.add_argument("input", nargs="?", help="source IWAD/PWAD")
    ap.add_argument("output", nargs="?", help="trimmed WAD to write")
    ap.add_argument("--adpcm", action="store_true",
                    help="keep DS* lumps but transcode them to IMA-ADPCM "
                         "(4:1 vs 16-bit PCM) and report stats")
    ap.add_argument("--keep-audio", action="store_true",
                    help="keep all audio lumps (only rewrites the WAD)")
    ap.add_argument("--drop-music-banks", action="store_true",
                    help="also drop GENMIDI/DMXGUS/DMXGUSC")
    ap.add_argument("--json", action="store_true", help="print JSON stats too")
    ap.add_argument("--verify", action="store_true",
                    help="re-read output and verify every kept lump")
    ap.add_argument("--no-verify", action="store_true")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.input or not args.output:
        ap.error("input and output are required (or use --self-test)")

    in_size = os.path.getsize(args.input)
    magic, lumps = read_wad(args.input)
    kept, dropped, _db, adpcm_stats = trim(
        magic, lumps, args.adpcm, args.keep_audio, args.drop_music_banks)
    write_wad(args.output, magic, kept)
    out_size = os.path.getsize(args.output)

    verification = "skipped"
    if not args.no_verify:
        verify_output(magic, lumps, args.output, kept, args.adpcm)
        verification = "OK"

    total_lumps = len(lumps)
    dropped_lumps = sum(dropped.values())
    dropped_bytes = 0
    for l in lumps:
        if classify(l.name, args.drop_music_banks) and not args.keep_audio:
            if args.adpcm and classify(l.name, args.drop_music_banks) == "sfx":
                continue
            dropped_bytes += len(l.data)
    ratio = (100.0 * out_size / in_size) if in_size else 0.0

    print("[wadtrim] input=%s lumps=%d bytes=%d (%s)"
          % (args.input, total_lumps, in_size, fmt_bytes(in_size)))
    print("[wadtrim] dropped: music=%d sfx=%d pcspk=%d bank=%d "
          "total_lumps=%d audio_bytes=%d"
          % (dropped["music"], dropped["sfx"], dropped["pcspk"],
             dropped["music-bank"], dropped_lumps, dropped_bytes))
    print("[wadtrim] output=%s lumps=%d bytes=%d (%s) ratio=%.2f%% "
          "saved=%d (%s)"
          % (args.output, len(kept), out_size, fmt_bytes(out_size), ratio,
             in_size - out_size, fmt_bytes(in_size - out_size)))
    print("[wadtrim] verify=%s" % verification)
    if args.adpcm:
        st = adpcm_stats
        if st["adpcm_lumps"]:
            ratio16 = (st["adpcm_pcm16_bytes"] / st["adpcm_out_bytes"]
                       if st["adpcm_out_bytes"] else 0.0)
            ratio8 = (st["adpcm_in_bytes"] / st["adpcm_out_bytes"]
                      if st["adpcm_out_bytes"] else 0.0)
            print("[wadtrim] adpcm: lumps=%d orig8=%d enc=%d skipped=%d "
                  "pcm16=%d  ratio_vs_16bit=%.2f:1 ratio_vs_dmx8=%.2f:1"
                  % (st["adpcm_lumps"], st["adpcm_in_bytes"],
                     st["adpcm_out_bytes"], st["adpcm_skipped"],
                     st["adpcm_pcm16_bytes"], ratio16, ratio8))
        else:
            print("[wadtrim] adpcm: no DMX v3 sfx lumps found")
    if args.json:
        print("[wadtrim-json] " + json.dumps({
            "input": args.input, "output": args.output,
            "lumps_in": total_lumps, "lumps_out": len(kept),
            "bytes_in": in_size, "bytes_out": out_size,
            "ratio_percent": round(ratio, 3),
            "dropped": dropped, "dropped_bytes": dropped_bytes,
            "adpcm": adpcm_stats, "verify": verification,
        }, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (WadError, OSError) as exc:
        print("[wadtrim] ERROR: %s" % exc, file=sys.stderr)
        sys.exit(1)
