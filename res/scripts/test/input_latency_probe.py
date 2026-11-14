#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT
"""Headless QEMU probe: Notepad/Explorer keystroke latency vs document size.

What it measures
----------------
The clients stamp the moment the FIRST key of a drain batch leaves the shared
keyboard ring, stamp again after the repaint, and print every 32 batches:

    [notepad] input_lat  n=32 p50=<us> p99=<us> qmax=<n>   # 取出 -> 画完
    [notepad] e2e_lat    n=32 p50=<us> p99=<us> skip=<n>    # 内核入环 -> 画完
    [notepad] render_lat n=32 p50=<us> p99=<us>             # 纯绘制
    [explorer] input_lat / render_lat ...

p99 of a 32-sample window is the window maximum - read it as "worst batch".
`qmax` is the largest number of events found already queued in the ring when a
batch started: 0/1 means input never waits for a frame, larger means it does.

The document-size sweep is the point: latency used to grow with the number of
glyphs on screen, so "1 line" and "20 lines" are measured separately
(--prefill-lines). Each prefilled line is --prefill-chars long.

No-dropped-keys evidence: after the measured keys the probe sends Ctrl+S and
compares Notepad's `[notepad] len=<chars> lines=<n>` with the number of
characters it injected. The save also prints the TTF cache hit rate
(`[notepad] ttf save: hit.. miss.. hit%%.. glyf fetch..`).

Usage (Windows):
  python res/scripts/test/input_latency_probe.py --label 20lines --prefill-lines 20
  python res/scripts/test/input_latency_probe.py --prefill-lines 1 --keys 40

Exit codes: 0 = PASS, 1 = missing data / length mismatch, 2 = env error.
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
ALPHABET = "abcdefghijklmnopqrstuvwxyz0123456789"


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

    def cmd(self, command: str, settle: float = 0.05) -> str:
        self.sock.sendall((command + "\r\n").encode())
        if settle:
            time.sleep(settle)
        return self.drain()

    def keys(self, names, settle: float = 0.06, group: int = 8) -> None:
        """Inject key names, `group` keys per monitor write (throughput)."""
        for i in range(0, len(names), group):
            body = "\n".join("sendkey %s" % k for k in names[i:i + group])
            self.cmd(body, settle)

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


def wait_for(path: str, pattern: str, timeout: float, proc) -> bool:
    rx = re.compile(pattern)
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None:
            return False
        if rx.search(read_serial(path)):
            return True
        time.sleep(0.15)
    return False


def find_lat(text: str, tag: str, kind: str):
    m = re.findall(r"\[%s\] %s n=(\d+) p50=(\d+)us p99=(\d+)us" % (tag, kind), text)
    if not m:
        return None
    n, p50, p99 = m[-1]
    return int(n), int(p50), int(p99)


def find_kv(text: str, pattern: str):
    m = re.findall(pattern, text)
    return m[-1] if m else None


def drive_keys(hmp, serial: str, proc, tag: str, want: int, gap: float,
               key_names, timeout: float = 60.0):
    """Inject keys until the client prints a latency summary; return (ok, sent)."""
    rx = re.compile(r"\[%s\] input_lat" % tag)
    # 只认**本次测量窗口新产生**的汇总行。Ctrl+S 会重置直方图，但串口里
    # 还留着 prefill 阶段的旧行；旧实现 rx.search() 一看到旧行就返回，
    # 只发 8 个键、还把冷启动 p50 当成了稳态数据。
    before = len(rx.findall(read_serial(serial)))
    sent = 0
    end = time.time() + timeout
    while sent < want * 6 and time.time() < end:
        hmp.cmd("sendkey %s" % key_names[sent % len(key_names)], gap)
        sent += 1
        if sent % 8 == 0 and len(rx.findall(read_serial(serial))) > before:
            return True, sent
    return len(rx.findall(read_serial(serial))) > before, sent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--qemu", default=r"C:\Program Files\qemu\qemu-system-x86_64.exe")
    ap.add_argument("--keys", type=int, default=40,
                    help="measured keystrokes (>=32 batches needed)")
    ap.add_argument("--gap", type=float, default=0.05)
    ap.add_argument("--prefill-lines", type=int, default=1)
    ap.add_argument("--prefill-chars", type=int, default=60)
    ap.add_argument("--skip-explorer", action="store_true")
    ap.add_argument("--wait", type=float, default=180.0)
    ap.add_argument("--label", default="")
    ap.add_argument("--port", type=int, default=0,
                    help="HMP telnet port (0 = pick a free one)")
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    iso = os.path.join(root, "SkylineSystem-x86_64.iso")
    disk = os.path.join(root, "disk.img")
    serial = os.path.join(root, "input_latency_serial.log")
    for p in (iso, disk, args.qemu):
        if not os.path.isfile(p):
            print("ENV ERROR: missing %s" % p)
            return 2
    # Windows 的 safe-delete 会拦截 os.remove（trash-failed），旧串口日志一旦
    # 残留就会污染延迟/len 断言。直接把文件截断即可：QEMU 的 -serial file:
    # 本身也是 O_TRUNC 打开。
    try:
        open(serial, "wb").close()
    except OSError:
        pass

    port = args.port
    if not port:
        # bind(0) 拿到的随机端口偶尔落在 Windows/Hyper-V 保留段里：脚本能
        # bind 成功，QEMU 却监听失败 -> HMP 连不上。挑一个低位可用端口并
        # 复检一次，最终回退到 smoke 门禁常用的 45670。
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
                # 只 bind 不够：Windows 的保留端口段能 bind 成功，QEMU 的
                # inet_listen() 却会报 "Failed to find an available port"。
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
    print("  HMP    : 127.0.0.1:%d" % port)

    qargs = [args.qemu, "-M", "q35", "-cpu", "max", "-m", "2048M", "-smp", "4",
             "-cdrom", iso,
             "-serial", "file:" + serial,
             "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
             "-display", "none", "-vga", "std", "-no-reboot", "-no-shutdown",
             "-drive", "file=%s,if=none,id=drive0,format=raw" % disk,
             "-device", "ide-hd,drive=drive0,bus=ide.0"]

    label = (" [%s]" % args.label) if args.label else ""
    print("INPUT LATENCY PROBE%s  lines=%d chars/line=%d keys=%d" %
          (label, args.prefill_lines, args.prefill_chars, args.keys))
    qerr = open(os.path.join(root, "input_latency_qemu_stderr.log"), "wb")
    proc = subprocess.Popen(qargs, cwd=root, stdout=qerr,
                            stderr=subprocess.STDOUT)
    # 与稳定门禁对齐：给 QEMU 一点时间把 telnet monitor 真正 listen 起来。
    time.sleep(1.5)
    if proc.poll() is not None:
        print("ENV ERROR: QEMU exited immediately "
              "(see input_latency_qemu_stderr.log)")
        return 2
    hmp = None
    rc = 1
    try:
        if not wait_for(serial, r"\[perf\] desktop ready in", args.wait, proc):
            print("FAIL: desktop never became ready")
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

        # ---- Notepad: focus, prefill N lines, then type --------------------
        hmp.cmd("sendkey ctrl-alt-n", 0.6)
        time.sleep(1.5)

        for ln in range(args.prefill_lines):
            pat = [ALPHABET[(ln * 7 + i) % len(ALPHABET)]
                   for i in range(args.prefill_chars)]
            hmp.keys(pat, 0.06, 8)
            hmp.keys(["ret"], 0.05, 1)
        time.sleep(0.8)
        prefill_chars = args.prefill_lines * args.prefill_chars + args.prefill_lines

        # QEMU 的 sendkey 组合键偶尔会把修饰键抬起事件丢掉：探针开头发过
        # ctrl-alt-n，若 ALT 粘在内核 mods 里，Notepad 会按设计忽略所有
        # Ctrl+Alt+*（含 Ctrl+S）-> len= 永远不打印。先补一次裸 alt 的
        # press/release 清粘性，再连发两次 Ctrl+S。
        def force_save():
            # 高速 prefill 之后键盘环还在排空，Ctrl+S 可能被淹没；重试到
            # 串口真的出现一条**新的** [notepad] len= 行为止。
            rx_len = re.compile(r"\[notepad\] len=\d+")
            before = len(rx_len.findall(read_serial(serial)))
            for _ in range(5):
                hmp.cmd("sendkey alt", 0.3)
                hmp.cmd("sendkey ctrl-s", 0.8)
                time.sleep(1.0)
                if len(rx_len.findall(read_serial(serial))) > before:
                    return True
            return False

        # 第一次 Ctrl+S: 把"铺文档"阶段的样本丢掉 (客户端保存后重置窗口),
        # 同时拿到 prefill 之后的字符数, 作为"无丢字"比对的基线。
        force_save()
        # 必须用两个捕获组：find_kv 对单捕获组返回的是字符串 "61"，
        # 再取 [0] 会变成字符 '6' -> want_len 少算一个数量级。
        base_len = find_kv(read_serial(serial),
                           r"\[notepad\] len=(\d+) lines=(\d+)")
        base_len = int(base_len[0]) if base_len else prefill_chars

        ok_np, sent_np = drive_keys(hmp, serial, proc, "notepad", args.keys,
                                    args.gap, list(ALPHABET))

        # Ctrl+S: 打印 len= 与 TTF 缓存统计 (也证明内容长度正确)
        force_save()
        text = read_serial(serial)
        want_len = base_len + sent_np
        got_len = find_kv(text, r"\[notepad\] len=(\d+) lines=(\d+)")

        # ---- Explorer (可选, 文档大小对它无意义) ---------------------------
        ok_ex = True
        if not args.skip_explorer:
            hmp.cmd("sendkey ctrl-alt-e", 0.6)
            wait_for(serial, r"\[explorer\] start content=", 30.0, proc)
            time.sleep(2.0)
            ok_ex, _ = drive_keys(hmp, serial, proc, "explorer", args.keys,
                                  args.gap, ["down"])

        text = read_serial(serial)
        np_in = find_lat(text, "notepad", "input_lat")
        np_rd = find_lat(text, "notepad", "render_lat")
        np_e2 = find_lat(text, "notepad", "e2e_lat")
        np_q = find_kv(text, r"\[notepad\] input_lat n=\d+ p50=\d+us p99=\d+us "
                             r"qmax=(\d+) e2e_skip=(\d+)")
        if np_q:
            np_q = "qmax=%s e2e_skip=%s" % (np_q[0], np_q[1])
        ttf = find_kv(text, r"\[notepad\] ttf save: hit=(\d+) miss=(\d+) hit%=(\d+) "
                            r"atlas glyphs=(\d+)/(\d+) evict=(\d+) \| "
                            r"glyf win=(\d+)KB res=(\d+) fetch=(\d+)")
        ex_in = find_lat(text, "explorer", "input_lat")
        ex_rd = find_lat(text, "explorer", "render_lat")

        def fmt(v):
            return ("n=%d p50=%dus p99=%dus" % v) if v else "MISSING"

        print("  notepad  input : %s %s" % (fmt(np_in), np_q or "qmax=?"))
        print("  notepad  render: %s" % fmt(np_rd))
        print("  notepad  e2e   : %s" % fmt(np_e2))
        if ttf:
            print("  ttf cache      : hit=%s miss=%s hit%%=%s glyphs=%s/%s evict=%s "
                  "glyf win=%sKB res=%s fetch=%s" % ttf)
        if not args.skip_explorer:
            print("  explorer input : %s" % fmt(ex_in))
            print("  explorer render: %s" % fmt(ex_rd))
        if got_len:
            ok_len = int(got_len[0]) == want_len
            print("  len            : got=%s lines=%s want=%d -> %s" %
                  (got_len[0], got_len[1], want_len,
                   "OK" if ok_len else "MISMATCH (dropped keys!)"))
        else:
            ok_len = False
            print("  len            : MISSING (no [notepad] len= marker)")

        rc = 0 if (ok_np and np_in and ok_len and
                   (args.skip_explorer or (ok_ex and ex_in))) else 1
        print("RESULT: %s" % ("PASS" if rc == 0 else "FAIL"))
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
    return rc


if __name__ == "__main__":
    sys.exit(main())
