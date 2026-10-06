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
| Disk image | `programs/Makefile` | copies `doom.elf`, and `doom1.wad` if supplied |

## Hook wiring

| PureDOOM hook | Skyline backend |
|---|---|
| `doom_set_malloc` | libc `malloc` / `free` (the 12 MB zone is one `sys_mmap` mode-2 mapping) |
| `doom_set_file_io` | libc `fopen/fread/fseek/ftell` + `fsize` for eof; **writes are refused** (libc has no `fwrite`), so no junk files are created |
| `doom_set_gettime` | TSC calibrated against one RTC second (`sys_time`, syscall 17) at startup |
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

1. Obtain the shareware IWAD yourself (see Licensing) and place it at
   `programs/PureDOOM/doom1.wad`.
2. Build as usual (`make cm`); `programs/Makefile` copies `doom.elf` into the
   image and, when the WAD is present, `doom1.wad` to `/mp/`.
3. Boot. The desktop only spawns the DOOM window when `/mp/doom1.wad` exists,
   so a missing WAD costs nothing at boot.

## Known limitations

- **No audio.** The kernel has no sound driver, so `doom_get_sound_buffer()`
  / `doom_tick_midi()` are never called; the sound and music menu entries are
  hidden with `DOOM_FLAG_HIDE_SOUND_OPTIONS | DOOM_FLAG_HIDE_MUSIC_OPTIONS`.
- **No mouse.** First version is keyboard-only; the PS/2 page is currently
  consumed by the desktop, so mouse-look needs a shared-ring agreement first.
- **Fixed 320x200.** `doom_set_resolution()` is a no-op upstream; higher
  resolution needs the engine's `SCREENWIDTH/HEIGHT` to stop being constants.
- **TCS fallback.** If the RTC does not tick during calibration the clock falls
  back to a nominal 3 GHz, which paces the game wrong but keeps it running.
- **Not benchmarked.** No frame-rate measurement yet under QEMU TCG.

## Licensing

`PureDOOM.h` carries the id Software DOOM source license and the repository
LICENSE is GPL-2.0; the shim links against it and is therefore tagged
**GPL-2.0-only**, unlike the rest of `programs/` (MIT). Linking the MIT libc
into a GPL-2.0 program is fine; the resulting `doom.elf` is GPL-2.0.

The **WAD is not part of the source license** — `doom1.wad` (shareware, ~4 MB)
is id Software game data and must not be committed. `programs/PureDOOM/.gitignore`
excludes `*.wad`.
