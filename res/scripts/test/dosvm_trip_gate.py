#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT
"""dosvm_trip_gate.py — QEMU gate for the reversible IA-32e exit.

Proves (or disproves) that one AP really leaves x86_64, runs a 16-bit DOS
payload in real mode, and returns to long mode while the BSP + desktop keep
running.  Evidence comes from the serial log, not from code inspection:

    [dosvm] ready: blob=... @0x70000 ...      (Tier1 init, trampoline installed)
    [dosvm] facade loaded (syscall 210/211)  (Tier2 .ko auto-loaded)
    [dosvm] trip begin: cpu=N ...            (syscall 211 -> 0xF0 IPI)
    [dosvm] trip OK: cpu=N gen=... trips=... (returned, magic matched)
    [dos] init: ...                          (L3 window client spawned)

A triple fault during the real-mode window shows up as a QEMU reset or as a
second boot banner; both are treated as FAIL.

Usage:
    python3 res/scripts/test/dosvm_trip_gate.py
    python3 res/scripts/test/dosvm_trip_gate.py --wait 90 --no-trigger
"""

import argparse
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

READY = re.compile(r"\[dosvm\] ready:")
FACADE = re.compile(r"\[dosvm\] facade loaded")
TRIP_BEGIN = re.compile(r"\[dosvm\] trip begin: cpu=(\d+)")
TRIP_OK = re.compile(r"\[dosvm\] trip OK: cpu=(\d+) gen=(\d+) trips=(\d+)")
TRIP_FAIL = re.compile(r"\[dosvm\] trip (TIMEOUT|FAILED|DISABLED)")
DOS_CLIENT = re.compile(r"\[dos\] init:")
FATAL = re.compile(
    r"(?i)(kernel panic|triple fault|system halted|#PF|general protection|"
    r"uncaught irq|stall watchdog|oom killer|assertion failed)"
)
BOOT_BANNER = re.compile(r"(?i)(limine|skylinesystem|initialized cpu)")
# Desktop main loop is up only once the WM has focused a window and the
# compositor perf line appears.  Sending keys before that lands in the void.
# TCG boot time varies a lot (Lua wallpaper + TTF + compositor warmup), so
# gate on a *late* marker rather than an early kmod line.
DESKTOP_UP = re.compile(r"\[wm\] focus pid=|\[perf\] loops=")


def pick_port():
    """Bind AND listen: Windows reserves random high ports for HMP."""
    for _ in range(64):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", 0))
            s.listen(1)
            return s.getsockname()[1]
        except OSError:
            continue
        finally:
            s.close()
    raise RuntimeError("no usable HMP port")


class Hmp:
    def __init__(self, port, timeout=3.0):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        # eat the greeting
        time.sleep(0.15)
        try:
            self.sock.recv(4096)
        except OSError:
            pass

    def cmd(self, text, wait=0.4):
        try:
            self.sock.sendall((text + "\r\n").encode())
        except OSError:
            return ""
        time.sleep(wait)
        out = b""
        try:
            while True:
                chunk = self.sock.recv(8192)
                if not chunk:
                    break
                out += chunk
                if len(out) > 65536:
                    break
        except OSError:
            pass
        return out.decode("utf-8", "replace")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def read_serial(path):
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=r"C:\Program Files\qemu\qemu-system-x86_64.exe")
    ap.add_argument("--iso", default=os.path.join(ROOT, "SkylineSystem-x86_64.iso"))
    ap.add_argument("--disk", default=os.path.join(ROOT, "disk.img"))
    ap.add_argument("--wait", type=float, default=75.0)
    ap.add_argument("--serial", default=os.path.join(ROOT, "dosvm_serial.log"))
    ap.add_argument("--no-trigger", action="store_true",
                    help="only check boot + init, do not open the DOS window")
    ap.add_argument("--screenshot", default="",
                    help="save a QEMU screendump (PPM) after a successful trip")
    ap.add_argument("--dump-shadow", action="store_true",
                    help="dump physical shadow-screen bytes via HMP for diagnosis")
    args = ap.parse_args(argv)

    for p in (args.iso, args.disk):
        if not os.path.isfile(p):
            print("MISSING: %s" % p)
            return 2
    # truncate instead of os.remove (Windows safe-delete can deny it)
    with open(args.serial, "wb"):
        pass

    port = pick_port()
    qargs = [args.qemu, "-M", "q35", "-cpu", "max", "-m", "2048M", "-smp", "4",
             "-cdrom", args.iso,
             "-serial", "file:" + args.serial,
             "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
             "-display", "none", "-vga", "std", "-no-reboot", "-no-shutdown",
             "-drive", "file=%s,if=none,id=drive0,format=raw" % args.disk,
             "-device", "ide-hd,drive=drive0,bus=ide.0"]
    print("[gate] QEMU port=%d wait=%.0fs" % (port, args.wait))
    proc = subprocess.Popen(qargs, cwd=ROOT, stdout=subprocess.DEVNULL,
                            stderr=subprocess.STDOUT)
    hmp = None
    triggered = False
    result = "TIMEOUT"
    detail = ""
    try:
        # phase 1: connect HMP and wait for boot + Tier1/Tier2 init
        deadline = time.time() + args.wait
        while time.time() < deadline:
            txt = read_serial(args.serial)
            if FATAL.search(txt):
                result, detail = "FAIL", "fatal in serial log"
                break
            if READY.search(txt) and FACADE.search(txt):
                break
            if proc.poll() is not None:
                result, detail = "FAIL", "QEMU exited during boot"
                break
            time.sleep(0.5)
        else:
            result, detail = "FAIL", "Tier1/Tier2 init markers not seen"

        if result == "TIMEOUT":
            if hmp is None:
                try:
                    hmp = Hmp(port)
                except OSError as exc:
                    print("[gate] HMP connect: %s" % exc)

            # Wait for the desktop main loop (WM focus / compositor perf line),
            # otherwise the keyboard ring is not yet being polled.
            if not args.no_trigger and hmp:
                up_deadline = time.time() + min(90.0, args.wait)
                saw_desktop = False
                while time.time() < up_deadline:
                    snap = read_serial(args.serial)
                    if DESKTOP_UP.search(snap):
                        saw_desktop = True
                        break
                    if proc.poll() is not None:
                        break
                    time.sleep(0.5)
                print("[gate] desktop_up=%s" % saw_desktop)
                # Extra settle: the compositor perf line can appear while WM
                # is still spawning its startup clients.
                time.sleep(3.0)
                # Ctrl+Alt+T opens the DOS window; its client fires syscall 211
                # once at startup and again on ENTER.  Retry a few times: the
                # shared keyboard ring can drop a combo under TCG load.
                for attempt in range(4):
                    print("[gate] sending ctrl-alt-t (attempt %d)" % (attempt + 1))
                    hmp.cmd("sendkey ctrl-alt-t", wait=0.7)
                    time.sleep(1.8)
                    if DOS_CLIENT.search(read_serial(args.serial)) or \
                       TRIP_BEGIN.search(read_serial(args.serial)):
                        break
                    hmp.cmd("sendkey ret", wait=0.4)
                    time.sleep(0.6)
                triggered = True

            deadline = time.time() + min(35.0, args.wait)
            while time.time() < deadline:
                txt = read_serial(args.serial)
                if TRIP_OK.search(txt):
                    m = TRIP_OK.search(txt)
                    result = "PASS"
                    detail = ("real-mode round trip returned: cpu=%s gen=%s trips=%s"
                              % m.groups())
                    # Give the L3 client one poll interval (~100ms) to fetch
                    # the new generation and repaint, then capture the screen.
                    time.sleep(1.2)
                    if args.dump_shadow and hmp:
                        # Physical 0x7F010 = shadow cells start (16B header).
                        dump = hmp.cmd("xp /96bx 0x7f010", wait=0.8)
                        print("[gate] shadow dump:\n" + dump[:1400])
                    if args.screenshot and hmp:
                        ppm = args.screenshot
                        with open(ppm, "wb"):
                            pass
                        hmp.cmd("screendump " + ppm, wait=1.2)
                        try:
                            print("[gate] screendump: %s (%d B)"
                                  % (ppm, os.path.getsize(ppm)))
                        except OSError:
                            print("[gate] screendump failed")
                    break
                if TRIP_FAIL.search(txt):
                    result = "FAIL"
                    detail = "trip reported failure: " + TRIP_FAIL.search(txt).group(0)
                    break
                if FATAL.search(txt):
                    result = "FAIL"
                    detail = "fatal during trip"
                    break
                if proc.poll() is not None:
                    result, detail = "FAIL", "QEMU exited during trip"
                    break
                time.sleep(0.4)
            else:
                if result == "TIMEOUT":
                    result, detail = "FAIL", "no [dosvm] trip OK marker within window"

        txt = read_serial(args.serial)
    finally:
        if hmp:
            try:
                hmp.cmd("quit", wait=0.2)
            except OSError:
                pass
            hmp.close()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()

    print("\n===== dosvm serial evidence =====")
    for line in txt.splitlines():
        if re.search(r"dosvm|\[dos\]|facade|triple|panic|uncaught", line, re.I):
            print("  " + line.strip()[:180])
    real_ok = "REAL" if TRIP_OK.search(txt) else "no trip"
    print("\n[gate] triggered=%s ready=%s facade=%s dos_client=%s %s"
          % (triggered, bool(READY.search(txt)), bool(FACADE.search(txt)),
             bool(DOS_CLIENT.search(txt)), real_ok))
    print("[gate] RESULT %s — %s" % (result, detail))
    print("[gate] serial: %s (%d B)" % (args.serial, len(txt)))
    # archive a failing serial log so a later run cannot overwrite evidence
    if result != "PASS":
        dst = os.path.join(ROOT, "dosvm_gate_fail_%d.log" % int(time.time()))
        try:
            with open(args.serial, "rb") as s, open(dst, "wb") as d:
                d.write(s.read())
            print("[gate] FAIL log archived: %s" % dst)
        except OSError:
            pass
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
