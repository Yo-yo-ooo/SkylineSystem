#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT
"""Headless QEMU gate: client spawn/exit churn (window open/close stress).

Why this gate exists (2026-11)
------------------------------
A kernel page fault (`rb_insert` on a corrupted VMA tree, CR2=0x8) showed up
roughly 1 in 16 runs of `start_explorer_smoke.py`, always on the *second*
Explorer launch - i.e. right after a client process had exited. Root cause:
`CleanPM()` freed the pagemap's VMA sentinel **page** with `PMM::Free`, but that
page is carved into N nodes by the global VMA node pool and half of them belong
to other pagemaps -> use-after-free -> the next `rb_insert` walked a recycled
node (`parent` RED, `parent->parent` == NULL) and the kernel halted.

A single spawn/exit per boot samples that bug once. This gate amplifies it:
it opens and closes a window N times **in one boot**, which is also the path
that exercises the WM's `sys_munmap` of the shared surface + protocol page
(before that fix, every open/close leaked 2 VMAs + ~1.2 MB in the desktop).

Checks
  C1  the desktop reaches "[perf] desktop ready in"
  C2  all N cycles spawn a client and it reports "[explorer] start"
  C3  all N cycles exit cleanly ("[explorer] exit")
  C4  the window really disappears ("[wm] child pid=<pid>" per cycle)
  C5  no panic / STALL WATCHDOG / #PF / OOM anywhere in the log
  C6  the last cycle still renders (screendump differs from the empty desktop)

Usage (Windows):
  python res/scripts/test/client_churn_gate.py             # 10 cycles
  python res/scripts/test/client_churn_gate.py --cycles 20

Exit codes: 0 = PASS, 1 = assertion failure, 2 = environment/setup failure.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", ".."))
FATAL = re.compile(r"(?i)(kernel panic|stall watchdog|#PF|kernel fault"
                   r"|general protection|oom killer|assertion failed"
                   r"|Segfault: unmapped|SYSTEM HALTED)")

FAIL = 0


def report(name: str, ok: bool, detail: str = "") -> None:
    global FAIL
    if not ok:
        FAIL += 1
    print("  %-4s %-52s %s" % ("PASS" if ok else "FAIL", name, detail))


class Hmp:
    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=10.0)
        self.sock.settimeout(0.05)

    def drain(self) -> str:
        out = b""
        while True:
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                out += chunk
            except socket.timeout:
                break
            except OSError:
                break
        return out.decode("utf-8", "replace")

    def cmd(self, command: str, settle: float = 0.06) -> str:
        self.sock.sendall((command + "\r\n").encode())
        if settle:
            time.sleep(settle)
        return self.drain()

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def read_serial(path: str) -> str:
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def wait_count(path: str, pattern: str, want: int, timeout: float, proc,
               poll: float = 0.1) -> tuple[bool, str]:
    rx = re.compile(pattern)
    end = time.time() + timeout
    last = ""
    while time.time() < end:
        if proc.poll() is not None:
            return False, last
        last = read_serial(path)
        if len(rx.findall(last)) >= want:
            return True, last
        time.sleep(poll)
    return False, last


def archive_fatal(serial: str, tag: str):
    """Keep an immutable copy whenever a fatal kernel marker is present.

    The canonical serial path is truncated at the start of every run, so a
    later PASS run used to silently destroy the evidence for an earlier
    STALL / #PF. That actually happened (see docs/desktop-features.md 15.4):
    a `STALL WATCHDOG` observed in churn cycle 8 was overwritten by the
    follow-up clean run, leaving the doc claim unreviewable. Archive BEFORE
    any later run can truncate this file.
    """
    txt = read_serial(serial)
    if not FATAL.search(txt):
        return None
    dst = os.path.join(os.path.dirname(serial),
                       "%s_%s_%d.log" % (tag, time.strftime("%Y%m%d-%H%M%S"),
                                         os.getpid()))
    try:
        shutil.copyfile(serial, dst)
        print("  FATAL SERIAL ARCHIVED: %s" % dst)
        return dst
    except OSError:
        return None


def pick_port() -> int:
    """bind + LISTEN precheck: Windows reserves some ephemeral port ranges."""
    for _ in range(16):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", 0))
            cand = s.getsockname()[1]
        except OSError:
            cand = 0
        finally:
            s.close()
        if not (1024 <= cand < 49152):
            continue
        t = socket.socket()
        try:
            t.bind(("127.0.0.1", cand))
            t.listen(1)
            return cand
        except OSError:
            pass
        finally:
            t.close()
    return 45670


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--qemu", default=r"C:\Program Files\qemu\qemu-system-x86_64.exe")
    ap.add_argument("--cycles", type=int, default=10)
    ap.add_argument("--wait", type=float, default=180.0)
    ap.add_argument("--action-wait", type=float, default=30.0)
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    iso = os.path.join(root, "SkylineSystem-x86_64.iso")
    disk = os.path.join(root, "disk.img")
    serial = os.path.join(root, "client_churn_serial.log")
    shot = os.path.join(root, "client_churn.ppm")
    for p in (iso, disk, args.qemu):
        if not os.path.isfile(p):
            print("ENV ERROR: missing %s" % p)
            return 2
    # Truncate the serial log instead of os.remove(): Windows safe-delete can
    # silently refuse the delete and leave a previous run's markers behind.
    try:
        open(serial, "wb").close()
    except OSError:
        pass
    try:
        os.remove(shot)
    except OSError:
        pass

    port = pick_port()

    qargs = [args.qemu, "-M", "q35", "-cpu", "max", "-m", "2048M", "-smp", "4",
             "-cdrom", iso,
             "-serial", "file:" + serial,
             "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
             "-display", "none", "-vga", "std", "-no-reboot", "-no-shutdown",
             "-drive", "file=%s,if=none,id=drive0,format=raw" % disk,
             "-device", "ide-hd,drive=drive0,bus=ide.0"]

    print("CLIENT SPAWN/EXIT CHURN (%d cycles)" % args.cycles)
    proc = subprocess.Popen(qargs, cwd=root, stdout=subprocess.DEVNULL,
                            stderr=subprocess.STDOUT)
    hmp = None
    try:
        ok, txt = wait_count(serial, r"\[perf\] desktop ready in", 1, args.wait,
                             proc, 0.4)
        report("C1) desktop ready", ok, "[perf] desktop ready in")
        if not ok:
            print("  (serial tail) %r" % txt[-500:])
            return 1
        for _ in range(20):
            try:
                hmp = Hmp("127.0.0.1", port)
                break
            except OSError:
                time.sleep(0.5)
        if hmp is None:
            print("ENV ERROR: cannot connect to HMP")
            return 2

        spawned = exited = reaped = 0
        for i in range(1, args.cycles + 1):
            # Ctrl+Alt+E: 直达 Explorer 的全局快捷键 (WM 内部会先关掉开始菜单,
            # 所以不存在"菜单开着/没开"的状态竞态 —— 用 Ctrl+Alt+B + Enter 的话,
            # 上一轮残留的菜单会把 Enter 吃掉, 造成假失败, 实测踩过)。
            hmp.cmd("sendkey ctrl-alt-e", 0.30)
            ok_s, txt = wait_count(serial, r"\[explorer\] start content=",
                                   spawned + 1, args.action_wait, proc)
            if ok_s:
                spawned += 1
            time.sleep(1.0)         # 等窗口起来并拿到键盘焦点
            ok_x = False
            for _ in range(2):      # Esc 关闭; 被别的窗口吃掉时重试一次
                hmp.cmd("sendkey esc", 0.20)
                ok_x, txt = wait_count(serial, r"\[explorer\] exit",
                                       exited + 1, 8.0, proc)
                if ok_x:
                    break
            if ok_x:
                exited += 1
            ok_r, txt = wait_count(serial, r"\[wm\] child pid=", reaped + 1,
                                   8.0, proc)
            if ok_r:
                reaped += 1
            if not (ok_s and ok_x):
                print("  cycle %d: spawn=%s exit=%s" % (i, ok_s, ok_x))
                break
            time.sleep(0.3)

        report("C2) every cycle spawned a client", spawned == args.cycles,
               "spawned=%d/%d" % (spawned, args.cycles))
        report("C3) every cycle exited cleanly", exited == args.cycles,
               "exited=%d/%d" % (exited, args.cycles))
        report("C4) WM reaped every child", reaped >= args.cycles - 1,
               "child-exit events=%d" % reaped)

        hmp.cmd("screendump " + shot.replace("\\", "/"), 1.2)

        txt = read_serial(serial)
        bad = FATAL.findall(txt)
        report("C5) no panic / STALL / #PF / OOM", len(bad) == 0,
               "matches=%s" % (",".join(sorted(set(x.lower() for x in bad))) or "0"))
        report("C6) last cycle still rendered",
               os.path.getsize(shot) > 3000000 if os.path.exists(shot) else False,
               "%s bytes" % (os.path.getsize(shot) if os.path.exists(shot) else 0))

        print("\nCLIENT CHURN: %s" % ("PASS" if FAIL == 0 else "FAIL"))
        return 0 if FAIL == 0 else 1
    finally:
        if hmp is not None:
            try:
                hmp.cmd("quit", 0.3)
            except OSError:
                pass
            hmp.close()
        if proc.poll() is None:
            time.sleep(0.5)
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        # Archive AFTER QEMU has stopped so the copy contains the whole log.
        archive_fatal(serial, "client_churn_fatal")


if __name__ == "__main__":
    sys.exit(main())
