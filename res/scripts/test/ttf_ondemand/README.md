# TTF on-demand loaders — host verification

Compares the three font loaders bit for bit:

| loader | what stays resident |
|---|---|
| `TTF_ReadFont()` | the whole file (baseline) |
| `TTF_ReadFontLazy()` | the 7 tables stb_truetype touches, `glyf` included |
| `TTF_ReadFontGlyfLazy()` | the 6 small tables only; `glyf` is fetched **per glyph** into a 512 KB sliding window |

Run from the repository root (the test uses a relative font path):

```sh
gcc -O2 -std=gnu17 -Ilib/include -Iablib -Iablib/freestndchdrs/x86_64/include \
    -include res/scripts/test/ttf_ondemand/ttfprelude.h \
    -c lib/base/font/ttf.c -o /tmp/ttf_host.o
gcc -O2 -std=gnu17 -Ilib/include -Iablib \
    -c res/scripts/test/ttf_ondemand/ttftest.c -o /tmp/ttftest.o
gcc -O2 -o /tmp/ttftest.exe /tmp/ttftest.o /tmp/ttf_host.o
./ttftest.exe
```

Two phases:

1. a short mixed ASCII/CJK string through all three loaders;
2. **2000 distinct CJK glyphs** — far more than the window holds, so the
   window must be recycled many times. This is the phase that would catch a
   bad eviction, a stale `loca` entry or a missed composite component.

Passing means **0 differing bytes** in both phases.

## Why `ttfprelude.h`

`ttf.c` is written against Skyline's libc, so the host build needs:

- `fsize()` — a Skyline extension, not in any host libc;
- `#define fopen(p, m) fopen(p, "rb")` — Skyline's `"r"` is plain `O_RDONLY`,
  whereas a host `"r"` does CRLF translation and `fread` then returns fewer
  bytes than asked for, which silently corrupts binary reads.
