# PureDOOM port (userspace DOOM client)

> Goal deliverable document. What was ported, how it is wired, and what is
> known-missing are all recorded truthfully.

PureDOOM (github.com/Daivuk/PureDOOM) ships the whole DOOM engine — renderer,
game logic and WAD loader — as one freestanding C header. This port runs it as
an ordinary SkylineSystem windowed client next to the console and Notepad.

## Components

| Component | Location | Description |
|---|---|---|
| Upstream header | `programs/PureDOOM/PureDOOM.h` | vendored single-header engine (1.3 MB), GPL-2.0 |
| Port shim | `programs/PureDOOM/skyline/doom_main.c` | the entire port: 6 hooks + keyboard pump + 2x blit |
| Build | `programs/PureDOOM/Makefile` | builds `programs/bin/doom.elf` (pinned `-std=gnu17`) |
| Spawn | `programs/desktop/main.cpp` | one `wm_client_spawn()` after the Notepad launch |
| Frame ping | `programs/desktop/synthesizer/wm_client.cpp` | `wm_clients_poll_output()` — per-client OUT_SEQ |
| Disk image | `programs/Makefile` | copies `doom.elf`, and `doom.wad` / `doom1.wad` if supplied |
| Host test | `programs/PureDOOM/hosttest/` | runs the shim unmodified on a desktop (no OS build) |

## Hook wiring

| PureDOOM hook | Skyline backend |
|---|---|
| `doom_set_malloc` | libc `malloc` / `free` (the 12 MB zone is one `sys_mmap` mode-2 mapping) |
| `doom_set_file_io` | libc `fopen/fread/fseek/ftell` + `fsize` for eof; **writes are refused** (libc has no `fwrite`), so no junk files are created |
| `doom_set_gettime` | RTC-anchored clock: whole seconds from `sys_time` (syscall 17), sub-second fraction from the TSC, with the TSC rate **re-measured across every real second** — self-recalibrating, no cumulative drift |
| `doom_set_getenv` | `DOOMWADDIR` and `HOME` both return `/mp` (`HOME` NULL makes DOOM `I_Error`) |
| `doom_set_exit` | `sys_exit` |
| `doom_set_print` | `sys_dbgsout` (serial) — diagnostics only, never drawn into the window |

Input: the shared keyboard ring (`dev_mmap(DEV_TYPE_KEYBOARD)`), same protocol
as Notepad. ASCII is passed through (uppercased letters are lowered), arrows
map to DOOM's `0xac..0xaf`, and Ctrl/Shift/Alt edges are synthesised from the
per-event modifier snapshot. Defaults are re-bound to WASD + E via
`doom_set_default_int`.

Output: `doom_get_framebuffer(4)` is 320x200 RGBA; the window body is 640x440,
so the WM hands the client a 640x400 content area and the shim does a
nearest-neighbour 2x upscale into the shared ARGB surface. The client bumps
`SKYWIN_PROTO_OUT_SEQ` after every frame, which is what makes the compositor
recompose in real time.

## Building and running

1. Obtain the IWAD yourself (see Licensing) and place it at
   `programs/PureDOOM/doom.wad` (registered, ~11 MB) or
   `programs/PureDOOM/doom1.wad` (shareware, ~4 MB).
2. Build as usual (`make cm`); `programs/Makefile` copies `doom.elf` into the
   image and, when an IWAD is present, it to `/mp/`.
3. Boot. The desktop only spawns the DOOM window when `/mp/doom.wad` (or `/mp/doom1.wad`) exists,
   so a missing WAD costs nothing at boot.

## Kernel bugs this port shook out (both fixed)

DOOM is the first Skyline userspace program that **probes for files** and
**seeks around inside a file**, and it tripped two latent kernel bugs:

1. **`sys_fopen` reported failed opens as success** (`fops.cpp`). The fs layer
   returns *positive* errno (lwext4: `EOK=0`, `ENOENT=2`) but the check was
   `err < 0`, so opening a missing file handed out a valid fd whose
   `ext4_file` was never opened; the next op hit
   `ext4_assert(file && file->mp)` and panicked the kernel. Now `err != 0`,
   with the positive errno normalised to negative for the syscall ABI.
   (`exec.cpp` / `task.cpp` already used `!= 0`.)

2. **Every read was served from offset 0** (`fops.cpp`, `fd.h`). The read path
   queried the position with `lseek(fd, 0, SEEK_CUR)`, but lwext4's
   `ext4_fseek()` returns a *status code* (`EOK=0`), not the new position — so
   the "current offset" was always 0 and the following
   `lseek(fd, cur_offset, SEEK_SET)` reset the file to the start. A single
   whole-file read from 0 still worked, which is why the 31 MB TTF font
   loaded fine and hid the bug. Fixed by tracking the offset in `fd_t::offset`
   (set by `sys_flseek`, advanced by `sys_fread`/`sys_fwrite`).

### Client build requirement: the linker script

Every windowed client must link with `-T ./link/console_x86_64.ld`. That script
reserves a **writable `.prepad` page at 0x400000**, which is the WM protocol
page (`SKYWIN_PROTO_PAGE_VA`) the desktop shares *before* the client starts.
Linking without it puts the ELF's own read-only header at 0x400000, and the
first `OUT_SEQ` bump dies with `write to RO` at `0x400048`. This is easy to
miss because Notepad never writes `OUT_SEQ`.

Diagnosing both relied on the serial log (`sys_dbgsout`) and a QEMU monitor
`screendump`; see `programs/PureDOOM/hosttest/` and `res/scripts/`.

## Resource loading: on demand

Both big assets are read on demand, not slurped into RAM up front:

| Asset | Before | Now |
|---|---|---|
| IWAD (`/mp/doom.wad`) | mirrored into RAM (11.2 MB) as a workaround for the broken VFS seek | read lump by lump through the VFS; DOOM's zone cache keeps the hot lumps and the kernel file cache serves the rest. The mirror still exists as `SKY_WAD_MIRROR_FALLBACK` (compile-time, default **0**) in case a kernel ever loses seek again |
| TTF (`SourceHanSerifTC`) | one 30 MB `fread` of the whole file | `TTF_ReadFontGlyfLazy()` keeps only the 6 small tables and streams `glyf` **one glyph at a time**. Callers: `lib/stdc/outfb/printf.c` (console) and `programs/notepad` |

### How per-glyph `glyf` works

`glyf` is 28.94 MB of the 30.13 MB font, so it cannot stay resident. The loader
still writes a glyf *directory record* into the font image, but it points at a
**sliding window** (512 KB) at the tail of the image:

1. `stb_truetype` resolves a glyph through `stbtt__GetGlyfOffset()` =
   `data + glyf + loca[gid]` on **every** access, so patching `loca` is
   enough — the rasterizer itself needed no changes.
2. On a miss the glyph's byte slice is read from the file into the window and
   its two `loca` entries are rewritten to address it. Composites pull their
   components in with them (depth-limited).
3. A non-resident glyph has its `loca` pair zeroed, which stbtt reads as
   "empty glyph" (-1) — never an out-of-bounds read.
4. When the window fills it is recycled; the window only grows if a single
   composite chain cannot fit.

The unpatched `loca` is mirrored once (`numGlyphs+1` uint32) so slices can be
located after patching. Short-loca fonts (glyf < 256 KB by definition) and TTC
collections fall back to `TTF_ReadFontLazy()` / `TTF_ReadFont()`.

| | per process | both processes |
|---|---|---|
| whole-file loader | 30.13 MB | 60.26 MB |
| per-glyph loader | **1.47 MB** (1.22 MB image + 0.25 MB loca mirror) | 2.94 MB |
| saved | 28.66 MB (**20.5x**) | **57.33 MB** |

`res/scripts/test/ttf_ondemand/` renders the same text through all three
loaders and compares the bitmaps byte by byte: short mixed string (3880 B) and
2000 distinct CJK glyphs (479700 B, forces many window recycles) — **0
differing bytes** in both. The 2000-glyph run reads 1.28 MB of outline out of
28.94 MB. `TTF_GetGlyfStats()` reports window cap / resident bytes / fetches.

## Known limitations (temporary)

- **No audio.** The kernel has no sound driver, so `doom_get_sound_buffer()`
  / `doom_tick_midi()` are never called; the sound and music menu entries are
  hidden with `DOOM_FLAG_HIDE_SOUND_OPTIONS | DOOM_FLAG_HIDE_MUSIC_OPTIONS`.
- **Mouse: look + fire only.** The client maps the shared PS/2 page
  (`graphic/mouseshare.h`) and feeds `doom_mouse_move()` / `doom_button_*()`
  while its window holds focus. The desktop still owns the pointer for
  dragging/resizing, so there is no grab (moving the mouse off the window just
  stops turning).
- **Window resize is "more room", not scaling.** The client's surface is
  allocated once at spawn size, so the WM mirrors the 640x400 bitmap 1:1 into
  its own presentation surface: shrinking crops, enlarging/maximizing leaves
  paper around it. Getting true scaling needs the client surface to be
  re-shared at the new size (protocol work, not done).
- **Fixed 320x200.** `doom_set_resolution()` is a no-op upstream; higher
  resolution needs the engine's `SCREENWIDTH/HEIGHT` to stop being constants.
- **Not benchmarked on target.** The host test proves the logic; guest frame
  rate under QEMU TCG is unmeasured. The clock self-recalibrates, so even a
  guest with a virtualised/odd TSC stays paced by the RTC.

## Verification (host run)

`programs/PureDOOM/hosttest/` runs `skyline/doom_main.c` unmodified on a
desktop by faking the protocol page, the keyboard ring and the syscalls.
With the real registered IWAD (`doom.wad`, 11.16 MB) on a Windows host — an earlier run with freedoom showed its own teal-green assets, which is what a too-green/cyan looking screen means (asset palette, not a channel bug) (gcc -O2):

- boot reaches `ST_Init`, IWAD (28.8 MB) loads through the file hooks;
- scripted ENTERs drive menu → New Game → skill → E1M1 through the
  keyboard-ring protocol;
- the game runs at a steady **35 tics/s** (DOOM's fixed rate);
- dumped frames (`frame_menu.png`, `frame_game.png`) show the skill menu and
  first-person E1M1 with the full HUD, correctly 2x-upscaled into the ARGB
  surface layout the WM shares.

The test also caught the one real bug so far: a one-shot TSC calibration ran
the game at ~228 tics/s (6.5x) on a machine whose TSC rate is not invariant —
fixed by the per-second re-anchoring clock described above.

## Licensing

`PureDOOM.h` carries the id Software DOOM source license and the repository
LICENSE is GPL-2.0; the shim links against it and is therefore tagged
**GPL-2.0-only**, unlike the rest of `programs/` (MIT). Linking the MIT libc
into a GPL-2.0 program is fine; the resulting `doom.elf` is GPL-2.0.

The **WAD is not part of the source license** — the IWAD
is id Software game data and must not be committed. `programs/PureDOOM/.gitignore`
excludes `*.wad`.
