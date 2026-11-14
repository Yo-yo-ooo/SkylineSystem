#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT
"""Headless gate: boot the ISO N times and require "[perf] desktop ready in".

Why: the 2026-11 bug was a *probabilistic* startup hang - a short/failed read of
/mp/libsky.so (1,032,200 B, the largest single read during boot) produced
"bad ELF header", the desktop process failed to load and the screen stayed on
the wallpaper. That kind of bug can only be closed by repetitive evidence.

For every boot it checks
  * "[perf] desktop ready in" appears within --wait seconds,
  * no "[dynlink] ... bad ELF header" / "prepare failed",
  * no "[elfimg] short read" (the retry path; a clean boot should not need it),
  * no kernel panic / STALL WATCHDOG / #PF.

Usage (Windows):
  python res/scripts/test/boot_stability_5x.py            # 5 boots
  python res/scripts/test/boot_stability_5x.py --runs 10

Exit codes: 0 = all boots ready, 1 = at least one boot failed, 2 = env error.
"""
from __future__ import annotations

import argparse
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", ".."))
BAD = re.compile(r"(?i)(bad ELF header|prepare failed|\[elfimg\] short read"
                 r"|kernel panic|stall watchdog|#PF|oom killer|assertion failed)")


def read(path: str) -> str:
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def one_boot(idx: int, qemu: str, iso: str, disk: str, wait: float) -> tuple[bool, str]:
    serial = os.path.join(ROOT, "boot_stability_%d.log" % idx)
    try:
        os.remove(serial)
    except OSError:
        pass
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    args = [qemu, "-M", "q35", "-cpu", "max", "-m", "2048M", "-smp", "4",
            "-cdrom", iso,
            "-serial", "file:" + serial,
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-display", "none", "-vga", "std", "-no-reboot", "-no-shutdown",
            "-drive", "file=%s,if=none,id=drive0,format=raw" % disk,
            "-device", "ide-hd,drive=drive0,bus=ide.0"]
    t0 = time.time()
    proc = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.DEVNULL,
                            stderr=subprocess.STDOUT)
    ok = False
    detail = ""
    try:
        end = t0 + wait
        while time.time() < end:
            if proc.poll() is not None:
                detail = "QEMU exited"
                break
            txt = read(serial)
            if "[perf] desktop ready in" in txt:
                ok = True
                break
            time.sleep(0.5)
        txt = read(serial)
        bad = BAD.findall(txt)
        if ok and bad:
            ok = False
            detail = "markers: %s" % ",".join(sorted(set(bad)))
        if ok:
            m = re.search(r"\[perf\] desktop ready in (\d+)ms", txt)
            detail = "ready in %sms, %.1fs wall" % (m.group(1) if m else "?",
                                                    time.time() - t0)
        elif not detail:
            detail = "timeout after %.0fs" % (time.time() - t0)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
        time.sleep(1.0)     # let QEMU release disk.img before the next boot
    return ok, detail


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--qemu", default=r"C:\Program Files\qemu\qemu-system-x86_64.exe")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--wait", type=float, default=120.0)
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    iso = os.path.join(root, "SkylineSystem-x86_64.iso")
    disk = os.path.join(root, "disk.img")
    for p in (iso, disk, args.qemu):
        if not os.path.isfile(p):
            print("ENV ERROR: missing %s" % p)
            return 2

    print("BOOT STABILITY (%d runs)" % args.runs)
    fails = 0
    for i in range(1, args.runs + 1):
        ok, detail = one_boot(i, args.qemu, iso, disk, args.wait)
        if not ok:
            fails += 1
        print("  %-4s boot #%d  %s" % ("PASS" if ok else "FAIL", i, detail))
    print("BOOT STABILITY: %s (%d/%d ready)" %
          ("PASS" if fails == 0 else "FAIL", args.runs - fails, args.runs))
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
