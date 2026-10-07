import socket, sys, time

# Minimal QEMU monitor (telnet) client: wait for the prompt, run commands.
# usage: python qemu_monitor.py <host> <port> cmd1 [cmd2 ...]

def recv_until(s, token, timeout=20):
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        try:
            s.settimeout(1.0)
            chunk = s.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
        if token in buf:
            break
    return buf

def main():
    host, port = sys.argv[1], int(sys.argv[2])
    cmds = sys.argv[3:]
    s = socket.create_connection((host, port), timeout=10)
    recv_until(s, b"(qemu)")
    out = b""
    for c in cmds:
        s.sendall(c.encode() + b"\n")
        out += recv_until(s, b"(qemu)", timeout=30)
        time.sleep(0.5)
    s.close()
    sys.stdout.write(out.decode("utf-8", "replace"))

if __name__ == "__main__":
    main()
