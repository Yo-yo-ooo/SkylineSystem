"""Compositor perf probe: boot, trigger full repaints, parse `[perf]` lines.

Sends Ctrl+Alt+B (start-menu toggle) several times, which forces full-frame
compose, then reads the `[perf] loops=... comp=N avg_us=U` lines emitted by
the desktop every ~250 ms and reports the average compose time.

Usage: python res/scripts/test/comp_perf_probe.py [--toggles N] [--label X]
"""
import argparse
import os
import re
import socket
import statistics
import subprocess
import sys
import time

ROOT = r"C:\ZSY\SkylineSystem"
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"


class Hmp:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)

    def cmd(self, text, settle=0.15):
        self.sock.sendall((text + "\n").encode())
        time.sleep(settle)
        try:
            self.sock.recv(65536)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--toggles", type=int, default=12)
    ap.add_argument("--label", default="run")
    ap.add_argument("--port", type=int, default=4666)
    args = ap.parse_args()

    iso = os.path.join(ROOT, "SkylineSystem-x86_64.iso")
    disk = os.path.join(ROOT, "disk.img")
    serial = os.path.join(ROOT, "comp_perf_%s.log" % args.label)
    for f in (serial,):
        try:
            os.unlink(f)
        except OSError:
            pass
    cmd = [
        QEMU, "-M", "q35", "-cpu", "max", "-m", "2048M", "-smp", "4",
        "-cdrom", iso,
        "-drive", "file=%s,if=none,id=d0,format=raw" % disk,
        "-device", "ide-hd,drive=d0,bus=ide.0",
        "-snapshot", "-display", "none",
        "-serial", "file:%s" % serial,
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % args.port,
        "-net", "nic", "-net", "user", "-no-reboot",
    ]
    proc = subprocess.Popen(cmd, cwd=ROOT)
    try:
        deadline = time.time() + 120
        while time.time() < deadline:
            if os.path.exists(serial):
                with open(serial, "rb") as f:
                    if b"desktop ready" in f.read():
                        break
            if proc.poll() is not None:
                print("qemu exited early")
                return 1
            time.sleep(0.2)
        else:
            print("desktop never became ready")
            return 1

        hmp = Hmp(args.port)
        hmp.cmd("", 0.2)
        time.sleep(0.5)
        for i in range(args.toggles):
            hmp.cmd("sendkey ctrl-alt-b", 0.35)
        # let the last perf window flush
        time.sleep(1.2)
        with open(serial, "r", errors="replace") as f:
            text = f.read()
        _ = re  # keep import style
        pat = re.compile(r"comp=(\d+) avg_us=(\d+)")
        vals = [int(m.group(2)) for m in pat.finditer(text) if int(m.group(1)) > 0]
        if not vals:
            print("no comp>0 perf samples")
            return 1
        vals = vals[-20:]
        print("%s: samples=%d median=%dus min=%dus max=%dus"
              % (args.label, len(vals), int(statistics.median(vals)),
                 min(vals), max(vals)))
        print("  values:", vals)
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
