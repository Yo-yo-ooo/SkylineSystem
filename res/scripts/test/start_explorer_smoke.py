#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT
"""Headless QEMU gate: Start menu + Explorer file browser.

What it proves (serial markers + screendump pixel diffs):

  A1  desktop reaches "[perf] desktop ready in"
  A2  Ctrl+Alt+B opens the Start menu (overlay really registered/drawn)
  A3  Enter on the first item launches Explorer through the Start menu
  A4  Explorer enumerated a *real* directory via syscall 23
      ("[explorer] readdir path=/mp ... source=syscall", rc >= 0)
  A5  Enter navigates into a directory ("[explorer] enter dir=...")
  A6  Backspace returns to the parent ("[explorer] up path=/mp")
  A7  Esc exits Explorer ("[explorer] exit"), WM reclaims the window
  A8  no panic / STALL WATCHDOG / OOM crash markers
  E1  Start menu screendump differs from the base frame (polled: the menu is
      composited by the 30 Hz heavy loop, so it can land 1-2 frames late)
  E2  Explorer screendump differs from the Start-menu frame
  M0  Explorer relaunched from the Start menu for the mouse phase
  M1  a real injected click (HMP mouse_move + mouse_button) is delivered to
      the client under the pointer: "[wm] mouse->client .." and
      "[explorer] mouse click idx=N"
  M2  a second click on the same row within 700 ms opens the entry
      ("[explorer] mouse open idx=N")
  M3  the keyboard path is untouched: Esc still exits ("[explorer] exit" x2)
  M4  no panic / STALL / #PF after real mouse input

Usage (Windows, from anywhere):
  python res/scripts/test/start_explorer_smoke.py
  python res/scripts/test/start_explorer_smoke.py --wait 180 --port 45678

Outputs (under the repo root):
  start_explorer_serial.log, start_explorer_base.ppm,
  start_explorer_menu.ppm, start_explorer_explorer.ppm,
  start_explorer_enter.ppm, start_explorer_mouse.ppm

Exit codes: 0 = PASS (warnings allowed), 1 = assertion failure,
            2 = environment/setup failure.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import re
import socket
import subprocess
import sys
import time

FAIL = 0
WARN = 0
SERIAL = ""


def report(name: str, ok: bool, detail: str = "", warn_only: bool = False) -> None:
    global FAIL, WARN
    tag = "PASS" if ok else ("WARN" if warn_only else "FAIL")
    if not ok:
        if warn_only:
            WARN += 1
        else:
            FAIL += 1
    print("  %-4s %-52s %s" % (tag, name, detail))


def read_serial(path: str) -> str:
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


FATAL = re.compile(
    r"(?i)(kernel panic|stall watchdog|#PF|general protection|oom killer"
    r"|assertion failed|SYSTEM HALTED|Segfault: unmapped)")


def archive_fatal(serial: str, tag: str):
    """Immutable copy of any run containing a fatal kernel marker.

    The canonical serial path is cleared at the start of every run, so a
    later clean run silently destroys the only evidence for an earlier
    #PF / STALL. Archive before that can happen.
    """
    if not FATAL.search(read_serial(serial)):
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


def wait_marker(path: str, pattern: str, timeout: float, proc) -> tuple[bool, str]:
    rx = re.compile(pattern)
    end = time.time() + timeout
    last = ""
    while time.time() < end:
        if proc.poll() is not None:
            return False, last
        last = read_serial(path)
        if rx.search(last):
            return True, last
        time.sleep(0.4)
    return False, last


def count_marker(text: str, pattern: str) -> int:
    return len(re.findall(pattern, text))


def wait_marker_count(path: str, pattern: str, want: int, timeout: float,
                      proc, poll: float = 0.3) -> tuple[bool, str]:
    """Wait until `pattern` has matched at least `want` times."""
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


def mouse_move_to(hmp, tx: int, ty: int, settle: float = 0.08) -> None:
    """Move the PS/2 pointer to absolute (tx,ty).

    HMP `mouse_move dx dy` injects *relative* deltas for a PS/2 mouse, and the
    kernel clamps the cursor to the screen, so: first slam it into the (0,0)
    corner with a few big negative moves, then walk to the target in <=120 px
    steps (the 9-bit signed delta limit).
    """
    for _ in range(8):
        hmp.cmd_fast("mouse_move -200 -200", settle)
    dx, dy = tx, ty
    while dx != 0 or dy != 0:
        sx = max(-120, min(120, dx))
        sy = max(-120, min(120, dy))
        hmp.cmd_fast("mouse_move %d %d" % (sx, sy), settle)
        dx -= sx
        dy -= sy
    time.sleep(0.25)      # 让 WM 的 30Hz 重活循环采到最终位置


def mouse_click(hmp, settle: float = 0.12) -> None:
    hmp.cmd_fast("mouse_button 1", settle)
    hmp.cmd_fast("mouse_button 0", settle)


class Hmp:
    """Minimal QEMU HMP telnet client (no negotiation needed for commands)."""

    def __init__(self, host: str, port: int, timeout: float = 10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(0.35)
        self.drain()

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

    def cmd(self, command: str, settle: float = 0.5) -> str:
        self.sock.sendall((command + "\r\n").encode())
        time.sleep(settle)
        return self.drain()

    def cmd_fast(self, command: str, settle: float = 0.08) -> str:
        """Inject an input event with a short settle + short drain timeout.

        The default `drain()` blocks on a 0.35 s socket timeout for every
        command, i.e. ~0.5 s per injected mouse event. A double click needs
        press/release/press inside the client's 700 ms window, so the input
        path uses this cheaper variant (~0.13 s per event).
        """
        self.sock.settimeout(0.05)
        try:
            self.sock.sendall((command + "\r\n").encode())
            time.sleep(settle)
            return self.drain()
        finally:
            self.sock.settimeout(0.35)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def ppm_diff(a: str, b: str) -> int:
    """Number of differing sample cells between two QEMU P6 screendumps."""
    try:
        with open(a, "rb") as fa, open(b, "rb") as fb:
            da, db = fa.read(), fb.read()
    except OSError:
        return 0
    if len(da) != len(db) or len(da) < 100:
        return 0
    # Header is identical for a fixed resolution; compare the RGB payload.
    marker = da.find(b"255\n")
    off = marker + 4 if marker >= 0 else 0
    step = 120  # ~every 40th pixel
    diffs = 0
    for p in range(off, min(len(da), len(db)) - 3, step):
        if da[p:p + 3] != db[p:p + 3]:
            diffs += 1
    return diffs


def main() -> int:
    global SERIAL
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(os.path.join(here, "..", "..", ".."))
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=root)
    ap.add_argument("--qemu", default=r"C:\Program Files\qemu\qemu-system-x86_64.exe")
    ap.add_argument("--iso", default="")
    ap.add_argument("--disk", default="")
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--wait", type=float, default=180.0)
    ap.add_argument("--action-wait", type=float, default=30.0)
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    iso = args.iso or os.path.join(root, "SkylineSystem-x86_64.iso")
    disk = args.disk or os.path.join(root, "disk.img")
    serial = os.path.join(root, "start_explorer_serial.log")
    shots = {k: os.path.join(root, "start_explorer_%s.ppm" % k)
             for k in ("base", "menu", "explorer", "enter", "mouse")}

    for p in (iso, disk, args.qemu):
        if not os.path.isfile(p):
            print("ENV ERROR: missing %s" % p)
            return 2

    port = args.port
    if not port:
        # Windows 的保留端口段能 bind 成功，QEMU 的 inet_listen() 却会报
        # "Failed to find an available port" 然后立刻退出（表现为 rc=1 且
        # 没有任何断言输出）。必须 bind + **listen** 预检，并多试几次。
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
                port = cand
                t.close()
                break
            except OSError:
                pass
            finally:
                try:
                    t.close()
                except OSError:
                    pass
        if not port:
            port = 45670

    # Truncate the serial log instead of os.remove(): Windows safe-delete can
    # silently refuse the delete and leave stale markers from a prior run.
    try:
        open(serial, "wb").close()
    except OSError:
        pass
    for p in shots.values():
        try:
            os.remove(p)
        except OSError:
            pass

    def wsl_path(p: str) -> str:
        """QEMU for Windows accepts forward-slash Windows paths in HMP."""
        return p.replace("\\", "/")

    qargs = [
        args.qemu, "-M", "q35", "-cpu", "max", "-m", "2048M", "-smp", "4",
        "-cdrom", iso,
        "-serial", "file:" + serial,
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
        "-display", "none", "-vga", "std", "-no-reboot", "-no-shutdown",
        "-drive", "file=%s,if=none,id=drive0,format=raw" % disk,
        "-device", "ide-hd,drive=drive0,bus=ide.0",
    ]
    print("START MENU / EXPLORER SMOKE")
    print("  root   : %s" % root)
    print("  serial : %s" % serial)
    print("  HMP    : 127.0.0.1:%d" % port)

    proc = None
    hmp = None
    try:
        proc = subprocess.Popen(qargs, cwd=root,
                                stdout=subprocess.DEVNULL,
                                stderr=subprocess.STDOUT)
        time.sleep(1.5)
        if proc.poll() is not None:
            print("ENV ERROR: QEMU exited immediately")
            return 2

        # ---- A1: desktop ready -------------------------------------------
        ok, SERIAL = wait_marker(serial, r"\[perf\] desktop ready in", args.wait, proc)
        report("A1) desktop ready", ok, "[perf] desktop ready in")
        if not ok:
            print("  (serial tail) %r" % SERIAL[-600:])
            return 1

        # ---- HMP connect + base frame ------------------------------------
        for _ in range(20):
            try:
                hmp = Hmp("127.0.0.1", port)
                break
            except OSError:
                time.sleep(0.5)
        if hmp is None:
            print("ENV ERROR: cannot connect to HMP monitor")
            return 2
        hmp.cmd("screendump " + wsl_path(shots["base"]), 1.5)

        # ---- A2: Ctrl+Alt+B opens the Start menu --------------------------
        hmp.cmd("sendkey ctrl-alt-b")
        ok, SERIAL = wait_marker(serial, r"\[wm\] start menu: open items=7",
                                 args.action_wait, proc)
        report("A2) Ctrl+Alt+B opens Start menu", ok, "[wm] start menu: open items=7")

        # ---- E1: menu really rendered -------------------------------------
        # 菜单是 30Hz 重活循环里合成的, 首次出现可能比串口标记晚 1~2 个
        # 重活周期 (启动阶段还要摆放 Notepad/Settings 两个窗口)。固定等 1.5s
        # 会在快机器上偶发拍到"菜单还没上屏"的那一帧, 所以轮询到帧变化为止。
        d = 0
        end = time.time() + min(args.action_wait, 12.0)
        while time.time() < end:
            hmp.cmd("screendump " + wsl_path(shots["menu"]), 1.0)
            d = ppm_diff(shots["base"], shots["menu"])
            if d > 20:
                break
            time.sleep(0.4)
        report("E1) Start menu changed the frame", d > 20,
               "diff cells=%d" % d)

        # ---- A3/A4: Enter launches Explorer and readdir works ------------
        hmp.cmd("sendkey ret")
        ok3, SERIAL = wait_marker(serial, r"\[wm\] start menu: launch=Explorer",
                                  args.action_wait, proc)
        report("A3) Start menu launches Explorer", ok3,
               "[wm] start menu: launch=Explorer")
        ok4, SERIAL = wait_marker(serial,
                                  r"\[explorer\] start content=\d+x\d+ cwd=/mp",
                                  args.action_wait, proc)
        report("A3b) Explorer window mounted", ok4, "[explorer] start")

        ok5, SERIAL = wait_marker(
            serial, r"\[explorer\] readdir path=/mp rc=\d+ entries=\d+ source=syscall",
            args.action_wait, proc)
        report("A4) real directory enumeration (syscall 23)", ok5,
               "readdir source=syscall rc>=0")
        if not ok5:
            m = re.findall(r"\[explorer\] readdir[^\r\n]*", SERIAL)
            print("  (fallback?) %r" % m[-2:])
        # Explorer 的 CLIENT_READY / 窗口注册 / 首帧合成比串口标记略晚，
        # 固定 sleep 或只截一次会在快机器上拍到"菜单帧"。轮询截图直到
        # 与菜单帧出现实质差异（最多 12s），再拿最终那一帧做证据。
        d = 0
        end = time.time() + min(args.action_wait, 12.0)
        while time.time() < end:
            hmp.cmd("screendump " + wsl_path(shots["explorer"]), 1.0)
            d = ppm_diff(shots["menu"], shots["explorer"])
            if d > 20:
                break
            time.sleep(0.5)
        # 再等一帧并补拍：客户端 surface 与合成器之间没有 flip/fence，
        # 首次出现差异的那一帧有可能仍是半合成状态，证据截图应取稳定帧。
        time.sleep(1.0)
        hmp.cmd("screendump " + wsl_path(shots["explorer"]), 1.0)
        d = max(d, ppm_diff(shots["menu"], shots["explorer"]))
        report("E2) Explorer window changed the frame", d > 20,
               "diff cells=%d" % d)

        # ---- A5: navigate into a directory -------------------------------
        hmp.cmd("sendkey ret")
        ok6, SERIAL = wait_marker(serial, r"\[explorer\] enter dir=",
                                  args.action_wait, proc)
        m = re.findall(r"\[explorer\] enter dir=([^\r\n]*)", SERIAL)
        report("A5) Enter opens a directory", ok6,
               ("dir=%s" % m[-1]) if m else "[explorer] enter dir=")
        hmp.cmd("screendump " + wsl_path(shots["enter"]), 1.5)

        # ---- A6: Backspace returns to parent -----------------------------
        hmp.cmd("sendkey backspace")
        ok7, SERIAL = wait_marker(serial, r"\[explorer\] up path=/mp",
                                  args.action_wait, proc)
        report("A6) Backspace returns to /mp", ok7, "[explorer] up path=/mp")

        # ---- A7: Esc exits Explorer --------------------------------------
        hmp.cmd("sendkey esc")
        ok8, SERIAL = wait_marker(serial, r"\[explorer\] exit",
                                  args.action_wait, proc)
        report("A7) Esc exits Explorer", ok8, "[explorer] exit")

        # ---- reopen the menu once more to prove it is reusable -----------
        hmp.cmd("sendkey ctrl-alt-b")
        n = count_marker(read_serial(serial), r"\[wm\] start menu: open items=7")
        report("A2b) Start menu can be reopened", n >= 2, "open count=%d" % n)
        hmp.cmd("sendkey esc")
        end = time.time() + args.action_wait
        closed = False
        while time.time() < end:
            if count_marker(read_serial(serial),
                            r"\[wm\] start menu: close") >= 2:
                closed = True
                break
            time.sleep(0.3)
        report("A2c) Esc closes Start menu", closed, "[wm] start menu: close")

        # ---- A8: no fatal kernel markers ---------------------------------
        SERIAL = read_serial(serial)
        bad = re.findall(r"(?i)(kernel panic|stall watchdog|#PF|general protection|oom killer|assertion failed)",
                         SERIAL)
        report("A8) no panic / STALL / #PF markers", len(bad) == 0,
               "matches=%d" % len(bad))

        # ================== 鼠标门禁 (P1: Explorer 可鼠标操作) ==================
        # Explorer 在 A7 已经退出, 这里用开始菜单重新拉起一个, 然后用 HMP 注入
        # 真实 PS/2 鼠标移动/点击 (不是伪造串口事件), 验证三件事:
        #   M1  WM 只把事件投给指针所在的那个客户端 ("[wm] mouse->client"),
        #       客户端把它换算成列表行 ("[explorer] mouse click idx=N");
        #   M2  700ms 内同一行的第二次点击 = 双击 = 打开该条目
        #       ("[explorer] mouse open idx=N");
        #   M3  键盘路径仍然可用 (Esc 退出 -> "[explorer] exit")。
        hmp.cmd("sendkey ctrl-alt-b")
        ok, SERIAL = wait_marker_count(serial, r"\[wm\] start menu: open items=7",
                                       3, args.action_wait, proc)
        report("M0) Start menu reopened for mouse phase", ok, "open count>=3")
        hmp.cmd("sendkey ret")
        ok, SERIAL = wait_marker_count(serial, r"\[wm\] start menu: launch=Explorer",
                                       2, args.action_wait, proc)
        report("M0b) Explorer relaunched", ok, "launch=Explorer x2")
        ok, SERIAL = wait_marker_count(serial, r"\[explorer\] start content=",
                                       2, args.action_wait, proc)
        report("M0c) Explorer mounted again", ok, "[explorer] start x2")
        time.sleep(1.2)

        # Explorer body = 640x460, WM 摆放 = 居中 + (dx=110, dy=50)
        #   body 左上 = ((1280-692)/2+110, (800-512)/2+50) = (404, 194)
        #   内容区原点 = body + (0, SKYWIN_TITLE_H=32)
        #   列表首行 y ≈ 内容区 88px (exp_layout: PAD16 + 标题行 + 分隔 + 路径栏)
        # 取内容区局部 (66,104): 落在第 1 行内, 又远离边框。
        #
        # 两次点击之间**不能**插任何等待/截图: 客户端的双击窗口是 700ms, 而一次
        # `screendump` 就要 1.5s、慢机上等一个串口标记也可能上秒 —— 那会把
        # "产品没问题"判成 FAIL (QA 复核时踩到过)。所以先连点两下, 再一起等标记。
        MX, MY = 470, 330
        mouse_move_to(hmp, MX, MY)
        mouse_click(hmp)          # 第一次点击 = 单击选中
        time.sleep(0.10)
        mouse_click(hmp)          # 第二次点击 = 双击打开 (与上一次相隔 ~0.25s)
        okOpen, SERIAL = wait_marker_count(
            serial, r"\[explorer\] mouse open idx=\d+", 1, 10.0, proc, 0.05)
        m = re.findall(r"\[explorer\] mouse click idx=(\d+)", SERIAL)
        okClick = len(m) >= 1
        okDeliver = count_marker(SERIAL, r"\[wm\] mouse->client pid=\d+") >= 1
        report("M1) click delivered to the client", okClick and okDeliver,
               "click idx=%s, [wm] mouse->client seen" % (m[-1] if m else "?"))

        # M2: 第二次点击必须命中同一行 (同一个 idx) 才算双击。
        mo = re.findall(r"\[explorer\] mouse open idx=(\d+)", SERIAL)
        same = bool(m and mo and m[-1] == mo[-1])
        report("M2) double click opens the entry", okOpen and same,
               "open idx=%s (same as click: %s)" % (mo[-1] if mo else "?", same))
        hmp.cmd("screendump " + wsl_path(shots["mouse"]), 1.2)

        # 键盘路径未受影响: Esc 仍然能退出 (若双击进了目录/预览, 先回到列表)
        tail = read_serial(serial)
        if "[explorer] preview path=" in tail.split("mouse open")[-1]:
            hmp.cmd("sendkey esc")
        elif "[explorer] enter dir=" in tail.split("mouse open")[-1]:
            hmp.cmd("sendkey backspace")
        time.sleep(0.8)
        hmp.cmd("sendkey esc")
        okExit, SERIAL = wait_marker_count(serial, r"\[explorer\] exit", 2,
                                           args.action_wait, proc)
        report("M3) keyboard Esc still exits Explorer", okExit,
               "[explorer] exit x2")

        SERIAL = read_serial(serial)
        bad = re.findall(r"(?i)(kernel panic|stall watchdog|#PF|general protection|oom killer|assertion failed)",
                         SERIAL)
        report("M4) no panic / STALL / #PF after mouse input", len(bad) == 0,
               "matches=%d" % len(bad))

        print("\nSTART MENU / EXPLORER SMOKE: %s (%d warning(s))" %
              ("PASS" if FAIL == 0 else "FAIL", WARN))
        return 0 if FAIL == 0 else 1
    finally:
        if hmp is not None:
            try:
                hmp.cmd("quit", 0.3)
            except OSError:
                pass
            hmp.close()
        if proc is not None and proc.poll() is None:
            time.sleep(0.5)
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        SERIAL = read_serial(serial)
        # Archive AFTER QEMU stopped so the copy contains the complete log.
        archive_fatal(serial, "start_explorer_fatal")


if __name__ == "__main__":
    sys.exit(main())
