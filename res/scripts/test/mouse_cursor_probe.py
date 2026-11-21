"""Mouse cursor probe: boot, inject a relative move, compare frames.

Boots the ISO headless with the QEMU HMP monitor on TCP, waits for the desktop,
takes a screendump, injects `mouse_move dx dy`, takes another screendump and
reports the connected components of the pixel diff. A working compositor cursor
shows up as two ~16x16 components (old and new position); a stuck/absent cursor
shows only clock/taskbar noise (or nothing).
"""
import os
import socket
import subprocess
import sys
import time

ROOT = r"C:\ZSY\SkylineSystem"
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
ISO = os.path.join(ROOT, "SkylineSystem-x86_64.iso")
DISK = os.path.join(ROOT, "disk.img")
SERIAL = os.path.join(ROOT, "mouse_probe_serial.log")


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


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", data[:2]
    idx = 2
    fields = []
    while len(fields) < 3:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] not in (b"\n", b""):
                idx += 1
            continue
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(data[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, data[idx:idx + w * h * 3]


def components(pa, pb, w, h):
    diff = bytearray(w * h)
    for i in range(w * h):
        o = i * 3
        if pa[o:o + 3] != pb[o:o + 3]:
            diff[i] = 1
    seen = bytearray(w * h)
    out = []
    for start in range(w * h):
        if not diff[start] or seen[start]:
            continue
        stack = [start]
        seen[start] = 1
        n = 0
        x0 = y0 = 10 ** 9
        x1 = y1 = -1
        while stack:
            p = stack.pop()
            n += 1
            x, y = p % w, p // w
            x0 = min(x0, x); x1 = max(x1, x)
            y0 = min(y0, y); y1 = max(y1, y)
            for q in (p - 1, p + 1, p - w, p + w):
                if 0 <= q < w * h and diff[q] and not seen[q]:
                    # do not wrap rows
                    if abs((q % w) - x) <= 1:
                        seen[q] = 1
                        stack.append(q)
        out.append((n, x0, y0, x1, y1))
    out.sort(reverse=True)
    return out


def main():
    if not os.path.isfile(ISO):
        print("missing ISO")
        return 2
    port = 4555
    for f in (SERIAL,):
        try:
            os.unlink(f)
        except OSError:
            pass
    cmd = [
        QEMU, "-M", "q35", "-cpu", "max", "-m", "2048M", "-smp", "4",
        "-cdrom", ISO,
        "-drive", "file=%s,if=none,id=d0,format=raw" % DISK,
        "-device", "ide-hd,drive=d0,bus=ide.0",
        "-snapshot", "-display", "none",
        "-serial", "file:%s" % SERIAL,
        "-monitor", "tcp:127.0.0.1:%d,server,nowait" % port,
        "-net", "nic", "-net", "user", "-no-reboot",
    ]
    proc = subprocess.Popen(cmd, cwd=ROOT)
    try:
        deadline = time.time() + 120
        while time.time() < deadline:
            if os.path.exists(SERIAL):
                with open(SERIAL, "rb") as f:
                    if b"desktop ready" in f.read():
                        break
            if proc.poll() is not None:
                print("qemu exited early")
                return 1
            time.sleep(0.2)
        else:
            print("desktop never became ready")
            return 1

        hmp = Hmp(port)
        hmp.cmd("", 0.2)
        # 光标线程在桌面 ready 后还要跑完首轮采样/首帧，留出暖机时间；
        # 单次 mouse_move 偶发丢包（PS/2 报文在启动窗口内被吞），因此
        # 分两步移动并对空 diff 重试。
        time.sleep(1.0)
        a = os.path.join(ROOT, "mouse_probe_a.ppm")
        b = os.path.join(ROOT, "mouse_probe_b.ppm")
        hmp.cmd("screendump %s" % a, 0.6)
        wa, ha, pa = read_ppm(a)
        comps = []
        for attempt in range(3):
            hmp.cmd("mouse_move 250 150", 0.45)
            hmp.cmd("mouse_move 250 150", 0.75)
            hmp.cmd("screendump %s" % b, 0.6)
            wb, hb, pb = read_ppm(b)
            comps = components(pa, pb, wa, ha)
            if comps:
                break
            time.sleep(0.4)
        print("frame", wa, ha, wb, hb)
        print("top diff components (pixels,x0,y0,x1,y1):")
        for c in comps[:8]:
            print("  ", c)
        with open(SERIAL, "rb") as f:
            log = f.read()
        print("first packet:", b"First packet" in log)
        return 0 if comps else 1
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    sys.exit(main())
