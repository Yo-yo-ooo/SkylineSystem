#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Yo-yo-ooo
# SPDX-License-Identifier: MIT
"""Headless QEMU gate: keystroke -> **visible glyph** latency in Notepad.

Why this exists
---------------
`input_latency_probe.py` measures how long the client needs to paint into its
own shared surface (microseconds). That is NOT what a user sees. The WM only
recomposes a client's surface when the client bumps SKYWIN_PROTO_OUT_SEQ;
before that fix Notepad/Explorer left the sequence at 0, so characters became
visible only on the 2 Hz taskbar clock fallback (~500 ms). This probe injects
a key and polls real `screendump` frames until the *typed glyph* is positively
confirmed on the text line.

Method (rewritten after an independent QA review found a self-confirming
low bias in the original version)
--------------------------------
* **Positive confirmation, not "any pixel changed".** A tight strip around
  the first text line is located automatically from a calibration keystroke.
  A frame counts only when it contains a LOCALIZED cluster of NEW ink pixels
  on that strip, not merely any differing pixel. In practice this cluster is
  the 2 px caret bar that `render_band()` repaints together with the newly
  typed glyph (`DrawFillRect(..., 2, ph, CARET)`); measured clusters are
  typically 2x17 px. That is a positive confirmation that THIS key's redraw
  reached the screen, but it is NOT OCR of the character. The old
  "hash of a large fixed box changed" criterion
  fired on unrelated redraws, which became *more* frequent after the very fix
  under test, so it systematically biased post-fix numbers low.
* **A fresh baseline frame is captured immediately before EVERY keystroke**,
  so redraws during the inter-key idle cannot produce a fake fast sample.
* **Each screendump is verified complete.** The target PPM is truncated to 0
  BEFORE the command is issued, then the file must grow to the exact full-PPM
  byte count (learned on the first dump; 1280x800 P6 = 3,072,016 B). No HMP
  reply is trusted as a barrier: a stale `info status` reply from the previous
  dump can arrive late and falsely "confirm" a half-written file. This is the
  bug that produced the implausible 2 ms readings in the first rewrite.
* **Two timestamps are reported.** `upper` = dump-completion time minus key
  issue: a rigorous UPPER bound on when the pixels were visible (it includes
  one full PPM write plus QEMU's HMP scheduling delay). `lower` = midpoint of
  the two surrounding dump ISSUE times: a LOOSE lower bound, because QEMU may
  not process a monitor command immediately after we send it. The gate uses
  `upper` so the result can never be flattered by an optimistic estimator;
  `lower` is printed only to show the width of the unresolved interval.
* **For continuity, the legacy "any pixel changed" criterion is measured in
  the same run**, so the size of the old bias is directly visible.

The gate is a *mechanism* check (frame-commit connected vs 2 Hz clock
fallback at ~500 ms), not an absolute latency SLA; the old absolute numbers
came from a biased probe and must not be reused as a threshold without
re-measuring both sides.

Usage:
  python res/scripts/test/display_latency_probe.py --label after
  python res/scripts/test/display_latency_probe.py --label after --samples 8

Exit codes: 0 = PASS (strict upper p50 < --p50-max, p99 < --p99-max,
            warmup max < --warm-max), 1 = too slow / no glyph seen,
            2 = environment error.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "..", ".."))
KEYS = ["a", "b", "c", "d", "e", "f", "g", "h"]

# Exact crop used by the original (biased) probe, kept ONLY so we can
# reproduce and quantify its criterion in the same boot.
LEGACY_BOX = (300, 200, 1100, 650)
# A pixel is "ink" when its luminance is below this. PAPER is near-white,
# INK/CARET are dark; the text background therefore contributes nothing.
DARK_LEVEL = 128
DIFF_LEVEL = 6
DARK_LUT = bytes(255 if v < DARK_LEVEL else 0 for v in range(256))
DIFF_LUT = bytes(255 if v >= DIFF_LEVEL else 0 for v in range(256))
# Smaller than any full 1280x800 P6 PPM; used only to learn the exact size on
# the very first dump. 3,072,016 B = 16 B header + 3 * 1280 * 800.
MIN_PPM = 1000000

FATAL = re.compile(
    r"(?i)(kernel panic|stall watchdog|#PF|system halted|oom killer)")


class Hmp:
    def __init__(self, host: str, port: int):
        self.sock = socket.create_connection((host, port), timeout=10.0)
        self.sock.settimeout(0.05)
        self.ppm_size = 0
        self.drain()

    def drain(self) -> str:
        out = b""
        while True:
            try:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                out += chunk
            except (socket.timeout, OSError):
                break
        return out.decode("utf-8", "replace")

    def cmd(self, command: str, settle: float = 0.04) -> str:
        self.sock.sendall((command + "\r\n").encode())
        if settle:
            time.sleep(settle)
        return self.drain()

    def send_key(self, key: str) -> float:
        """Inject a key WITHOUT a fixed settle; return its issue timestamp.

        The original probe slept a hard-coded 20 ms inside cmd() after t0,
        which put a constant +20 ms into every sample. HMP commands are
        processed immediately on loopback, so t0 right after sendall() is a
        fair start. Stale prompt bytes are drained first.
        """
        self.sock.settimeout(0.001)
        try:
            self.drain()
        finally:
            self.sock.settimeout(0.05)
        self.sock.sendall(("sendkey " + key + "\r\n").encode())
        return time.perf_counter()

    def shot(self, path: str, timeout: float = 1.0):
        """Take a *provably complete* screendump. Returns (ok, t_issue, t_done).

        The file is truncated to zero first, so ANY non-zero content belongs
        to this dump. QEMU must grow it to the exact full-PPM byte count
        before we read it. An earlier version used a trailing `info status`
        HMP command as a barrier; a late reply from the previous dump could
        satisfy that barrier and let a half-written PPM be decoded, producing
        false ~2 ms samples. Size is the only unambiguous completion signal.
        """
        try:
            with open(path, "wb"):
                pass
        except OSError:
            return False, time.perf_counter(), time.perf_counter()
        try:
            self.sock.sendall(("screendump " + path + "\r\n").encode())
        except OSError:
            return False, time.perf_counter(), time.perf_counter()
        t_issue = time.perf_counter()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                size = os.path.getsize(path)
            except OSError:
                size = 0
            if self.ppm_size == 0 and size >= MIN_PPM:
                # Header is complete by now; derive the exact final size.
                self.ppm_size = ppm_expected_size(path)
            if self.ppm_size and size == self.ppm_size:
                t_done = time.perf_counter()
                self._drain_prompt()
                return True, t_issue, t_done
            time.sleep(0.001)
        return False, t_issue, time.perf_counter()

    def _drain_prompt(self) -> None:
        """Drop HMP prompts; screendump itself has no textual reply."""
        self.sock.settimeout(0.001)
        try:
            self.drain()
        finally:
            self.sock.settimeout(0.05)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def ppm_expected_size(path: str) -> int:
    """Exact byte count of a P6 PPM, derived from its own header.

    `P6\n<W> <H>\n<maxval>\\n` is followed by 3*W*H raw bytes. Using the
    header avoids mistaking a paused partial write for the finished file and
    works at any guest resolution.
    """
    try:
        with open(path, "rb") as fh:
            hdr = fh.read(64)
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", hdr)
        if not m:
            return 0
        return m.end() + 3 * int(m.group(1)) * int(m.group(2))
    except (OSError, ValueError):
        return 0


def read_serial(path: str) -> str:
    try:
        with open(path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def wait_marker(path: str, pattern: str, timeout: float, proc) -> bool:
    rx = re.compile(pattern)
    end = time.time() + timeout
    while time.time() < end:
        if proc.poll() is not None:
            return False
        if rx.search(read_serial(path)):
            return True
        time.sleep(0.1)
    return False


def load_rgb(path: str):
    from PIL import Image
    with Image.open(path) as im:
        return im.convert("RGB")          # detached copy, safe after close


def frame_stats(path: str, strict_box, legacy_box):
    """One decode -> legacy hash, strict-box hash, ink mask."""
    rgb = load_rgb(path)
    return {
        "rgb": rgb,
        "legacy_hash": hashlib.md5(rgb.crop(legacy_box).tobytes()).hexdigest(),
        "strict_hash": hashlib.md5(rgb.crop(strict_box).tobytes()).hexdigest(),
        "ink": rgb.crop(strict_box).convert("L").point(DARK_LUT).tobytes(),
    }


def new_ink_stats(base: bytes, cur: bytes, sw: int):
    """Count and bounding box of pixels that BECAME ink vs the fresh baseline.

    A typed glyph + its caret produce a small localized dark cluster. A
    status-bar / window / taskbar redraw produces either a wide or a tall
    cluster, or nothing at all on this strip. This is the positive
    confirmation that replaces the old "hash differs" test.
    """
    n = 0
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for idx, (a, b) in enumerate(zip(base, cur)):
        if b and not a:
            n += 1
            x = idx % sw
            y = idx // sw
            if x < x0:
                x0 = x
            if x > x1:
                x1 = x
            if y < y0:
                y0 = y
            if y > y1:
                y1 = y
    if n == 0:
        return 0, 0, 0
    return n, x1 - x0 + 1, y1 - y0 + 1


def locate_text_line(base_rgb, cur_rgb, line_h: int):
    """Find a tight strip around the first text line, using one real key.

    The taskbar (bottom 60 px, 2 Hz clock) is excluded from the diff so the
    clock cannot define the box. Only the TOP of the change cluster is used:
    render_band() also repaints the status bar, which would otherwise make
    the box span the whole window.
    """
    from PIL import ImageChops
    diff = ImageChops.difference(base_rgb, cur_rgb).convert("L").point(DIFF_LUT)
    w, h = diff.size
    upper = diff.crop((0, 0, w, max(1, h - 60)))
    box = upper.getbbox()
    if not box:
        return None
    x0, y0, x1, _ = box
    sx0 = max(0, x0 - 16)
    sy0 = max(0, y0 - 4)
    sx1 = min(w, max(x1 + 240, x0 + 260))
    sy1 = min(h, sy0 + line_h)
    if sx1 <= sx0 or sy1 <= sy0:
        return None
    return (sx0, sy0, sx1, sy1), box


def pick_port() -> int:
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


def percentile(sorted_vals, q: float) -> float:
    if not sorted_vals:
        return float("nan")
    return sorted_vals[min(len(sorted_vals) - 1,
                           int(len(sorted_vals) * q))]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=ROOT)
    ap.add_argument("--qemu", default=r"C:\Program Files\qemu\qemu-system-x86_64.exe")
    ap.add_argument("--label", default="")
    ap.add_argument("--samples", type=int, default=8)
    ap.add_argument("--warmup", type=int, default=2,
                    help="first N keystrokes are reported separately "
                         "(cold glyph cache), not part of the steady state")
    ap.add_argument("--wait", type=float, default=180.0)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--timeout-ms", type=float, default=2500.0)
    ap.add_argument("--line-h", type=int, default=32,
                    help="height of the text-line strip, px")
    ap.add_argument("--box", default="",
                    help="override the auto-detected strict box x0,y0,x1,y1")
    ap.add_argument("--min-ink", type=int, default=16,
                    help="min NEW ink pixels for a positive glyph confirmation")
    ap.add_argument("--max-span", type=int, default=64,
                    help="max width of the NEW ink cluster, px")
    ap.add_argument("--p50-max", type=float, default=350.0)
    ap.add_argument("--p99-max", type=float, default=450.0)
    ap.add_argument("--warm-max", type=float, default=450.0)
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    iso = os.path.join(root, "SkylineSystem-x86_64.iso")
    disk = os.path.join(root, "disk.img")
    serial = os.path.join(root, "display_latency_serial.log")
    shot = os.path.join(root, "display_latency_frame.ppm")
    for p in (iso, disk, args.qemu):
        if not os.path.isfile(p):
            print("ENV ERROR: missing %s" % p)
            return 2
    try:
        open(serial, "wb").close()      # truncate, NOT os.remove (Windows)
    except OSError:
        pass

    lines = []

    def say(text: str = "") -> None:
        print(text)
        lines.append(text)

    label = (" [%s]" % args.label) if args.label else ""
    stamp = time.strftime("%Y%m%d-%H%M%S")
    out_path = os.path.join(root, "display_latency_report_%s_%s.txt" %
                            (args.label or "run", stamp))

    port = args.port or pick_port()
    qargs = [args.qemu, "-M", "q35", "-cpu", "max", "-m", "2048M", "-smp", "4",
             "-cdrom", iso, "-serial", "file:" + serial,
             "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
             "-display", "none", "-vga", "std", "-no-reboot", "-no-shutdown",
             "-drive", "file=%s,if=none,id=drive0,format=raw" % disk,
             "-device", "ide-hd,drive=drive0,bus=ide.0"]
    say("DISPLAY LATENCY PROBE%s  HMP=127.0.0.1:%d" % (label, port))
    say("  criterion: NEW ink >= %d px in a localized cluster "
        "(width <= %d px, height <= %d px) on the auto-located text line" %
        (args.min_ink, args.max_span, args.line_h + 6))
    qerr = open(os.path.join(root, "display_latency_qemu_stderr.log"), "wb")
    proc = subprocess.Popen(qargs, cwd=root, stdout=qerr,
                            stderr=subprocess.STDOUT)
    hmp = None
    try:
        time.sleep(1.5)
        if proc.poll() is not None:
            say("ENV ERROR: QEMU exited immediately")
            return 2
        if not wait_marker(serial, r"\[perf\] desktop ready in", args.wait, proc):
            say("FAIL: desktop never became ready")
            return 1
        for _ in range(20):
            try:
                hmp = Hmp("127.0.0.1", port)
                break
            except OSError:
                time.sleep(0.5)
        if hmp is None:
            say("ENV ERROR: cannot connect to HMP")
            return 2

        hmp.cmd("sendkey ctrl-alt-n", 0.6)
        if not wait_marker(serial, r"\[notepad\] start content=", 30.0, proc):
            say("FAIL: Notepad did not start")
            return 1
        time.sleep(1.5)

        wp = shot.replace("\\", "/")

        # ---- calibration: locate the text line from ONE real keystroke ----
        ok, t_prev, _ = hmp.shot(wp)
        if not ok:
            say("ENV ERROR: cannot read screendump (calibration)")
            return 2
        say("  full PPM size learned: %d bytes" % hmp.ppm_size)
        cal_base = load_rgb(wp)
        t_cal0 = hmp.send_key("a")
        t_prev = max(t_prev, t_cal0)
        cal_box = None
        cal_dt = None
        end = t_cal0 + args.timeout_ms / 1000.0
        while time.perf_counter() < end:
            ok, t_now, _ = hmp.shot(wp)
            if not ok:
                continue
            cur = load_rgb(wp)
            found = locate_text_line(cal_base, cur, args.line_h)
            if found:
                cal_box, diff_box = found
                cal_dt = ((t_prev + t_now) * 0.5 - t_cal0) * 1000.0
                break
            t_prev = t_now
        if args.box:
            try:
                cal_box = tuple(int(v) for v in args.box.split(","))
                if len(cal_box) != 4:
                    raise ValueError
            except ValueError:
                say("ENV ERROR: --box must be x0,y0,x1,y1")
                return 2
            say("  strict box overridden by --box: %s" % (cal_box,))
        elif cal_box is None:
            say("ENV ERROR: could not locate the Notepad text line")
            return 2
        else:
            say("  calibration key 'a': %.1f ms (loose lower bound), "
                "full-frame diff bbox=%s" % (cal_dt or -1.0, diff_box))
            say("  auto text-line strip: %s (taskbar + status bar excluded)" %
                (cal_box,))
        legacy_box = LEGACY_BOX
        sw = cal_box[2] - cal_box[0]
        time.sleep(0.2)

        # Poll-overhead calibration uses the SAME shot()+frame_stats() path as
        # the measurement loop, so the number is directly comparable.
        t_probe = time.perf_counter()
        for _ in range(10):
            hmp.shot(wp)
            frame_stats(wp, cal_box, legacy_box)
        poll_ms = (time.perf_counter() - t_probe) * 100.0
        say("  probe poll overhead: %.1f ms/sample (verified dump+decode)" %
            poll_ms)

        strict_s, legacy_s, any_s = [], [], []
        strict_w, legacy_w, any_w = [], [], []
        est_s = []
        for i in range(args.samples + args.warmup):
            k = KEYS[i % len(KEYS)]
            # B-2: fresh, complete baseline IMMEDIATELY before the key.
            ok, t_prev, _ = hmp.shot(wp)
            if not ok:
                say("  sample %d: ENV ERROR baseline dump failed" % (i + 1))
                (strict_w if i < args.warmup else strict_s).append(
                    args.timeout_ms)
                continue
            base = frame_stats(wp, cal_box, legacy_box)
            t0 = hmp.send_key(k)
            t_prev = max(t_prev, t0)
            dt_strict = dt_legacy = dt_any = None
            est_strict = None
            seen = (0, 0, 0)
            peak = (0, 0, 0)
            end = t0 + args.timeout_ms / 1000.0
            while time.perf_counter() < end:
                ok, t_now, t_done = hmp.shot(wp)
                if not ok:
                    t_prev = t_now
                    continue
                # `upper` is a rigorous upper bound: the framebuffer already
                # contained the glyph when QEMU finished writing this dump.
                upper = (t_done - t0) * 1000.0
                # `est` removes the PPM write by using dump ISSUE times.
                est = ((t_prev + t_now) * 0.5 - t0) * 1000.0
                s = frame_stats(wp, cal_box, legacy_box)
                if dt_legacy is None and s["legacy_hash"] != base["legacy_hash"]:
                    dt_legacy = upper
                if dt_any is None and s["strict_hash"] != base["strict_hash"]:
                    dt_any = upper
                if dt_strict is None:
                    n, spanx, spany = new_ink_stats(base["ink"], s["ink"], sw)
                    if n > peak[0]:
                        peak = (n, spanx, spany)
                    if (n >= args.min_ink and spanx <= args.max_span
                            and spany <= args.line_h + 6):
                        dt_strict = upper
                        est_strict = est
                        seen = (n, spanx, spany)
                t_prev = t_now
                if dt_strict is not None:
                    break
            is_warm = i < args.warmup
            tag = "warm  " if is_warm else "steady"
            cap = args.timeout_ms
            d_s = dt_strict if dt_strict is not None else cap
            d_l = dt_legacy if dt_legacy is not None else cap
            d_a = dt_any if dt_any is not None else cap
            (strict_w if is_warm else strict_s).append(d_s)
            (legacy_w if is_warm else legacy_s).append(d_l)
            (any_w if is_warm else any_s).append(d_a)
            if not is_warm and est_strict is not None:
                est_s.append(est_strict)
            if dt_strict is None:
                say("  %s %2d: NO GLYPH CONFIRMED within %.0f ms "
                    "(legacy=%.1f, any-strict=%.1f; peak ink/span=%d/%dx%d, "
                    "needs >=%d / <=%dx%d)" %
                    (tag, i + 1, cap, d_l, d_a, peak[0], peak[1], peak[2],
                     args.min_ink, args.max_span, args.line_h + 6))
            else:
                say("  %s %2d: strict=%.1f ms (lower %.1f)  legacy=%.1f  "
                    "any-strict=%.1f  [ink=%d span=%dx%d]" %
                    (tag, i + 1, d_s, est_strict, d_l, d_a,
                     seen[0], seen[1], seen[2]))
            time.sleep(0.15)

        strict_s.sort()
        legacy_s.sort()
        any_s.sort()
        est_s.sort()
        p50 = percentile(strict_s, 0.50)
        p99 = percentile(strict_s, 0.99)
        l50 = percentile(legacy_s, 0.50)
        a50 = percentile(any_s, 0.50)
        say("")
        say("  STRICT upper bound (positive glyph confirmation) "
            "p50=%.1f ms  p99=%.1f ms (n=%d)" % (p50, p99, len(strict_s)))
        if est_s:
            say("  STRICT lower bound (loose, HMP scheduling) p50=%.1f ms  "
                "p99=%.1f ms" %
                (percentile(est_s, 0.50), percentile(est_s, 0.99)))
        else:
            say("  STRICT lower bound: n/a (no glyph confirmed)")
        say("  LEGACY (any pixel in fixed box)      p50=%.1f ms  p99=%.1f ms" %
            (l50, percentile(legacy_s, 0.99)))
        say("  ANY-STRICT (any pixel in tight strip) p50=%.1f ms  p99=%.1f ms" %
            (a50, percentile(any_s, 0.99)))
        say("  criterion+crop bias (legacy - strict) p50: %+.1f ms" %
            (l50 - p50))
        wmax = max(strict_w) if strict_w else 0.0
        say("  warmup strict max: %.1f ms (n=%d); legacy %.1f ms, "
            "any-strict %.1f ms" %
            (wmax, len(strict_w),
             max(legacy_w) if legacy_w else 0.0,
             max(any_w) if any_w else 0.0))
        say("  timing resolution: %.1f ms/poll; `upper` includes one full "
            "PPM write and is conservative." % poll_ms)

        # This gate separates "OUT_SEQ frame commit connected" from the 2 Hz
        # taskbar-clock fallback (~500 ms). It is deliberately NOT an
        # absolute latency SLA: the previous absolute thresholds were
        # calibrated on a probe with a known low bias.
        ok = p50 < args.p50_max and p99 < args.p99_max and wmax < args.warm_max
        say("RESULT: %s (strict upper p50<%.0f / p99<%.0f, warmup max<%.0f ms; "
            "2 Hz clock fallback ~500 ms)" %
            ("PASS" if ok else "FAIL", args.p50_max, args.p99_max,
             args.warm_max))
        return 0 if ok else 1
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
        # Never lose a fatal kernel log to the next run's truncation again.
        try:
            if FATAL.search(read_serial(serial)):
                dst = os.path.join(root, "display_latency_serial_fatal_%s.log"
                                   % stamp)
                shutil.copyfile(serial, dst)
                say("  fatal markers found: serial archived to %s" % dst)
        except OSError:
            pass
        say("  report: %s" % out_path)
        try:
            with open(out_path, "w", encoding="utf-8") as fh:
                fh.write("\n".join(lines) + "\n")
        except OSError:
            pass


if __name__ == "__main__":
    sys.exit(main())
