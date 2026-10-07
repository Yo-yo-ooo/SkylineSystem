# hosttest — run the shim on any desktop (no OS build needed)

The shim is platform logic only; everything Skyline-specific is behind six
hooks and three calls (`fsize`, `sys_*`, `syscall`). `harness.c` fakes those
on a stock desktop so `skyline/doom_main.c` runs **unmodified** under any
x86_64 C compiler — which makes the port testable without WSL/QEMU.

## What it fakes

| Skyline piece | Host stand-in |
|---|---|
| fixed protocol page at `0x400000` | `VirtualAlloc` at that address (Windows) |
| shared window surface (692x492 ARGB) | heap buffer, geometry filled into the protocol page |
| `sys_dbgsout` (24) | `stderr` |
| `sys_time` (17) | `time(0)` |
| `sys_dev_mmap` (keyboard page) | heap `KbdShared`, keys injected by script |
| `sys_yield` | the frame pump: injects ENTERs, dumps frames, exits |
| `/mp/...` paths | rewritten to a local directory (see `MP_REAL`) |

`syscall.h` here shadows Skyline's `<syscall.h>` via include order — do NOT
add `-Ilib/include/base/arch/x86_64` when building the harness.

## How to run

```sh
# 1. put the IWAD in the directory MP_REAL points at (see harness.c).
#    Registered DOOM ships as doom.wad (~11 MB), shareware as doom1.wad
#    (~4 MB); the engine detects the game mode from which file it finds, so
#    keep the original name.
# 2. build and run (mingw/gcc example):
gcc -O2 -std=gnu17 -Dmain=doom_client_main \
    -Ihosttest -I.. -I../../../lib/include \
    -c ../skyline/doom_main.c -o doom_shim.o
gcc -O2 -std=gnu17 -Ihosttest -I.. -I../../../lib/include \
    -c harness.c -o harness.o
gcc -O2 -o doomtest.exe harness.o doom_shim.o
./doomtest.exe            # auto-plays: menu -> new game -> skill -> E1M1
# 3. convert the dumps (raw 640x400 BGRA-order ARGB rows):
python raw2png.py frame_menu.raw frame_menu.png 640 400
python raw2png.py frame_game.raw frame_game.png 640 400
```

Expected console output ends with `tics/s=35` repeatedly (DOOM's fixed game
rate) and two dumped frames; `frame_game.png` shows first-person E1M1 with
the full HUD. The committed `frame_menu.png` / `frame_game.png` are from an
actual run (freedoom 0.13 IWAD, Windows host, gcc -O2).

## What it proved / what it caught

- end-to-end: WAD load through the file hooks, zone allocator through the
  malloc hooks, menu navigation through the keyboard-ring protocol, level
  load, software renderer + palette + HUD, 2x blit into the ARGB surface.
- **caught a real bug**: the first clock used a one-shot TSC calibration,
  and on a machine with a non-invariant TSC the game ran at ~228 tics/s
  (6.5x speed). The shim now re-anchors on every RTC second boundary and
  re-measures the TSC rate per second (see `sky_gettime`), giving a steady
  35 tics/s.
