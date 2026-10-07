import sys, zlib, struct

def read_raw(path):
    with open(path, 'rb') as f:
        return f.read()

def write_png(path, w, h, data):
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)
        raw += data[y * stride:(y + 1) * stride]
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    hdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    out = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', hdr)
           + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(out)

def stats(w, h, data):
    nz = 0
    colors = {}
    total = w * h
    for i in range(0, len(data), 4):
        r, g, b = data[i], data[i + 1], data[i + 2]
        if r or g or b:
            nz += 1
        colors[(r, g, b)] = colors.get((r, g, b), 0) + 1
    return nz, total, len(colors), sorted(colors.items(), key=lambda kv: -kv[1])[:6]

src, dst = sys.argv[1], sys.argv[2]
w, h = int(sys.argv[3]), int(sys.argv[4])
d = read_raw(src)
write_png(dst, w, h, d)
nz, total, nc, top = stats(w, h, d)
print(f"{src}: non-black {nz}/{total} ({100.0*nz/total:.1f}%), distinct colors {nc}")
print("top colors:", [(f"#{r:02x}{g:02x}{b:02x}", c) for (r, g, b), c in top])
