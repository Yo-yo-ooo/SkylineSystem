# PureDOOM port

PureDOOM (github.com/Daivuk/PureDOOM) ships the whole engine — renderer, game logic, WAD loader —
as a single freestanding C header. This port runs it as an ordinary windowed client next to the
console and Notepad.

## Pieces

| Piece | Location | What it does |
|---|---|---|
| Upstream header | `programs/PureDOOM/PureDOOM.h` | vendored single-header engine (1.3 MB), GPL-2.0 |
| Port shim | `programs/PureDOOM/skyline/doom_main.c` | the whole port: 6 hooks, keyboard pump, 2x blit |
| Build | `programs/PureDOOM/Makefile` | produces `programs/bin/doom.elf` (pinned `-std=gnu17`) |
| Spawn | `programs/desktop/main.cpp` | one `wm_client_spawn()` after Notepad starts |
| Frame ping | `programs/desktop/synthesizer/wm_client.cpp` | `wm_clients_poll_output()`, per-client OUT_SEQ |
| Disk image | `programs/Makefile` | copies `doom.elf`, plus `doom.wad` / `doom1.wad` when supplied |
| Host test | `programs/PureDOOM/hosttest/` | runs the shim unmodified on a desktop, no OS build |

## Hooks

| PureDOOM hook | Backend |
|---|---|
| `doom_set_malloc` | libc `malloc`/`free`; the 12 MB zone is a single `sys_mmap` mode-2 mapping |
| `doom_set_file_io` | libc `fopen/fread/fseek/ftell` plus `fsize` for eof detection. Writes are refused (there is no `fwrite`), so no junk files get created |
| `doom_set_gettime` | RTC-anchored clock: whole seconds from `sys_time` (syscall 17), sub-second from the TSC, with the TSC rate re-measured across every real second. Self-recalibrating, no cumulative drift |
| `doom_set_getenv` | `DOOMWADDIR` and `HOME` both return `/mp`; a NULL `HOME` makes DOOM call `I_Error` |
| `doom_set_exit` | `sys_exit` |
| `doom_set_print` | `sys_dbgsout` to serial, diagnostics only, never drawn into the window |

Input comes from the shared keyboard ring (`dev_mmap(DEV_TYPE_KEYBOARD)`), same protocol as Notepad.
ASCII passes through with uppercase folded to lowercase, arrows map to DOOM's `0xac..0xaf`, and
Ctrl/Shift/Alt edges are synthesised from the per-event modifier snapshot. Defaults are rebound to
WASD + E through `doom_set_default_int`.

Output: `doom_get_framebuffer(4)` gives 320x200 RGBA. The window body is 640x440, so the WM hands the
client a 640x400 content area and the shim does a nearest-neighbour 2x upscale into the shared ARGB
surface. The client bumps `SKYWIN_PROTO_OUT_SEQ` after each frame, which is what keeps the compositor
redrawing in real time.

## Build and run

1. Get an IWAD yourself (see Licensing) and put it at `programs/PureDOOM/doom.wad` (registered,
   ~11 MB) or `programs/PureDOOM/doom1.wad` (shareware, ~4 MB).
2. `make cm`. `programs/Makefile` copies `doom.elf` into the image and, if an IWAD is present, copies
   it to `/mp/`.
3. Boot. The desktop only spawns the DOOM window when `/mp/doom.wad` or `/mp/doom1.wad` exists, so a
   missing WAD costs nothing.

## Two kernel bugs this port exposed

DOOM was the first Skyline program that probes for files and seeks around inside one, and it hit two
latent kernel bugs. Both are fixed.

1. `sys_fopen` reported failed opens as success (`fops.cpp`). The fs layer returns positive errno
   values (lwext4: `EOK=0`, `ENOENT=2`), but the check was `err < 0`, so opening a missing file handed
   out a valid fd whose `ext4_file` had never been opened. The next operation hit
   `ext4_assert(file && file->mp)` and panicked the kernel. The check is `err != 0` now, with positive
   errno normalised to negative for the syscall ABI. (`exec.cpp` and `task.cpp` always used `!= 0`.)
2. Every read was served from offset 0 (`fops.cpp`, `fd.h`). The read path asked for the position
   with `lseek(fd, 0, SEEK_CUR)`, but lwext4's `ext4_fseek()` returns a status code (`EOK=0`), not the
   new position. So the "current offset" was always 0, and the following `lseek(fd, cur_offset,
   SEEK_SET)` rewound the file. One whole-file read from 0 still worked, which is why loading the 31 MB
   TTF font hid the bug. The offset now lives in `fd_t::offset`, set by `sys_flseek` and advanced by
   `sys_fread`/`sys_fwrite`.

### The linker script clients must use

Every windowed client links with `-T ./link/console_x86_64.ld`. That script reserves a writable
`.prepad` page at 0x400000, which is the WM protocol page (`SKYWIN_PROTO_PAGE_VA`) the desktop
shares before the client starts. Link without it and the ELF's read-only header lands at 0x400000;
the first `OUT_SEQ` bump then dies with `write to RO` at `0x400048`. Easy to miss, because Notepad
never writes `OUT_SEQ`.

Diagnosis of both bugs came from the serial log (`sys_dbgsout`) plus a QEMU monitor `screendump`; see
`programs/PureDOOM/hosttest/` and `res/scripts/`.

## Loading big assets on demand

| Asset | Before | Now |
|---|---|---|
| IWAD (`/mp/doom.wad`) | mirrored into RAM (11.2 MB) to work around the broken VFS seek | read lump by lump through the VFS; DOOM's zone cache holds the hot lumps and the kernel file cache serves the rest. The mirror still exists as `SKY_WAD_MIRROR_FALLBACK` (compile-time, default 0) in case a kernel ever loses seek again |
| TTF (`SourceHanSerifTC`) | one 30 MB `fread` of the whole file | `TTF_ReadFontGlyfLazy()` keeps the 6 small tables and streams `glyf` one glyph at a time. Callers: `lib/stdc/outfb/printf.c` (console) and `programs/notepad` |

### Per-glyph glyf

`glyf` is 28.94 MB of the 30.13 MB font, so it can't stay resident. The loader still writes a glyf
directory record into the font image, but it points at a sliding 512 KB window at the tail of the
image.

1. `stb_truetype` resolves a glyph through `stbtt__GetGlyfOffset()` = `data + glyf + loca[gid]` on
   every access, so patching `loca` is enough; the rasterizer needed no changes.
2. On a miss the glyph's byte slice is read into the window and its two `loca` entries are rewritten
   to address it. Composites pull their components in with them, depth-limited.
3. A non-resident glyph gets its `loca` pair zeroed, which stbtt reads as "empty glyph" (-1), never
   an out-of-bounds read.
4. When the window fills it's recycled; it only grows if one composite chain can't fit.

The unpatched `loca` is mirrored once (`numGlyphs+1` uint32) so slices can still be located after
patching. Short-loca fonts (glyf under 256 KB by definition) and TTC collections fall back to
`TTF_ReadFontLazy()` / `TTF_ReadFont()`.

|  | per process | both processes |
|---|---|---|
| whole-file loader | 30.13 MB | 60.26 MB |
| per-glyph loader | 1.47 MB (1.22 MB image + 0.25 MB loca mirror) | 2.94 MB |
| saved | 28.66 MB (20.5x) | 57.33 MB |

`res/scripts/test/ttf_ondemand/` renders the same text through all three loaders and compares bitmaps
byte for byte: a short mixed string (3880 B) and 2000 distinct CJK glyphs (479700 B, enough to force
many window recycles). Zero differing bytes in both. The 2000-glyph run reads 1.28 MB of outline out
of 28.94 MB. `TTF_GetGlyfStats()` reports window cap, resident bytes and fetch count.

## Known limits

- No audio. The kernel has no sound driver, so `doom_get_sound_buffer()` and `doom_tick_midi()` are
  never called, and the sound/music menu entries are hidden with
  `DOOM_FLAG_HIDE_SOUND_OPTIONS | DOOM_FLAG_HIDE_MUSIC_OPTIONS`.
- Mouse: look and fire only. The client maps the shared PS/2 page (`graphic/mouseshare.h`) and feeds
  `doom_mouse_move()` / `doom_button_*()` while its window has focus. The desktop still owns the
  pointer for dragging and resizing, so there's no grab; moving off the window just stops turning.
- Window resize gives more room, it doesn't scale. The client surface is allocated once at spawn size,
  so the WM mirrors the 640x400 bitmap 1:1 into its presentation surface: shrinking crops, enlarging or
  maximizing leaves paper around it. True scaling needs the client surface re-shared at the new size,
  which is protocol work nobody has done.
- Fixed 320x200. `doom_set_resolution()` is a no-op upstream; higher resolution requires the engine's
  `SCREENWIDTH/HEIGHT` to stop being constants.
- Not benchmarked on target. The host test proves the logic; guest frame rate under QEMU TCG is
  unmeasured. The clock re-anchors every second, so an odd virtualised TSC still gets paced by the RTC.

## Host verification

`programs/PureDOOM/hosttest/` runs `skyline/doom_main.c` unmodified on a desktop by faking the
protocol page, the keyboard ring and the syscalls. With the real registered IWAD (`doom.wad`,
11.16 MB) on a Windows host at `gcc -O2`:

- boot reaches `ST_Init` and the IWAD (28.8 MB) loads through the file hooks;
- scripted ENTERs drive menu → New Game → skill → E1M1 through the keyboard-ring protocol;
- the game holds a steady 35 tics/s, DOOM's fixed rate;
- dumped frames (`frame_menu.png`, `frame_game.png`) show the skill menu and first-person E1M1 with the
  full HUD, correctly 2x-upscaled into the ARGB layout the WM shares.

An earlier run with freedoom showed its own teal-green assets; that colour is the asset palette, not a
channel bug.

The test also caught the one real bug so far: a one-shot TSC calibration ran the game at ~228 tics/s
(6.5x) on a machine without an invariant TSC. The per-second re-anchoring clock above is the fix.

## Licensing

`PureDOOM.h` carries the id Software DOOM source license; the repository LICENSE is GPL-2.0. The shim
links against the header and is therefore GPL-2.0-only, unlike the rest of `programs/` (MIT). Linking
the MIT libc into a GPL-2.0 program is fine; the resulting `doom.elf` is GPL-2.0.

The WAD is not covered by the source license. The IWAD is id Software game data and must not be
committed; `programs/PureDOOM/.gitignore` excludes `*.wad`.

## 2026-11: DOOM_TINY compression mode

`DOOM_TINY` (default 1 in `programs/PureDOOM/Makefile`) is documented in
[doom-extreme.md](doom-extreme.md). Summary:

- Framebuffer aliasing: `screens[0..3]` share 1×64KiB; wipe, save/load and the back screen are
  disabled.
- `screen_buffer` (64KiB) and `final_screen_buffer` (256KiB RGBA) are gone; the shim does a
  palette→ARGB LUT straight into the WM surface each frame.
- Fixed buffers drop from 640KiB to 65KiB, saving 575KiB.
- Audio is stripped entirely (tiny audio stub), and `tools/wadtrim.py` removes 289 audio lumps:
  `doom.wad 11,159,840 → 9,071,551 B` (81.29%).
- `--adpcm` IMA-ADPCM 4-bit path: 122 DMX sfx `1,222,616 → 620,982 B` (3.93:1 against 16-bit PCM,
  1.97:1 against DMX8).
- Zone policy under DOOM_TINY: ≤80MiB → 8MiB, ≤128MiB → 10MiB, >192MiB → 12MiB.
- QEMU gates at 2G and 64M both PASS (`res/scripts/test/doom_compact_smoke.ps1`); at 64M it runs
  zone=8, reaches `init done`, keeps the heartbeat, zero panics.

Limits worth stating: the WAD still streams from ext4 `/mp/doom.wad`, fixed_t and level data are not
lossily compressed, and this is not RP2040's 384KB environment.
