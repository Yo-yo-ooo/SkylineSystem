//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
//
// doom_main.c �?PureDOOM host shim for SkylineSystem.
//
// PureDOOM ships the whole game (renderer, game logic, WAD loader) as one
// freestanding C header; everything platform-shaped is funnelled through six
// doom_set_*() hooks. This file is the entire port: it wires those hooks to
// Skyline's libc/syscalls, pumps the keyboard ring into DOOM's event queue,
// and blits the 320x200 RGBA framebuffer into the ARGB surface the window
// manager shared with us (see graphic/winstyle.h).
//
// WAD discovery: this PureDOOM revision does NOT implement "-iwad"; it only
// honours $DOOMWADDIR (and -file). So the getenv hook must return "/mp" and
// the IWAD must live there (registered /mp/doom.wad, shareware /mp/doom1.wad).
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <base/base.h>
#include <graphic/winstyle.h>
#include <graphic/kbdshare.h>
#include <graphic/mouseshare.h>

#define DOOM_IMPLEMENTATION
#include "PureDOOM.h"

#define DOOM_SCR_W 320
#define DOOM_SCR_H 200

/* ---- DOOM_TINY compact presentation -------------------------------------
   The compact build keeps a single 8-bit palette-index screen: no 64 KiB copy
   buffer and no 256 KiB RGBA buffer. Build a 256-entry ARGB LUT from the
   engine's live palette (it changes on damage/pickup/radiation) and convert
   indices straight into the WM surface. Normal builds keep the RGBA path. */
#if DOOM_TINY
static uint32_t g_argb_lut[256];
static void sky_build_palette_lut(void) {
    const unsigned char* pal = doom_get_palette();
    for (int i = 0; i < 256; i++)
        g_argb_lut[i] = 0xFF000000u |
                        ((uint32_t)pal[i * 3 + 0] << 16) |
                        ((uint32_t)pal[i * 3 + 1] << 8) |
                        (uint32_t)pal[i * 3 + 2];
}
#endif

/* ---- serial debug (syscall 24) ----------------------------------------- */
static void sky_dbg(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    if (n) syscall(SYSCALL_DBGSOUT, (uint64_t)s, (uint64_t)n, 0, 0, 0, 0);
}

/* ---- memory ------------------------------------------------------------ */
static void* sky_malloc(int size) { return malloc((size_t)size); }
static void  sky_free(void* p)    { free(p); }

/* ---- file I/O ----------------------------------------------------------
   libc exposes fopen/fread/fseek/ftell/fsize but no fwrite, so writes are
   refused outright (returning NULL from open keeps DOOM from creating empty
   default.cfg stubs on the ext4 image). */
static int sky_ends_with(const char* s, const char* suf) {
    size_t ls = strlen(s), lt = strlen(suf);
    if (lt > ls) return 0;
    return strcmp(s + (ls - lt), suf) == 0;
}

/* DEFENSIVE: DOOM probes six IWAD names (doom2f.wad, doom2.wad, plutonia.wad,
   tnt.wad, doomu.wad, doom.wad, ...) before it finds ours, i.e. it opens
   files that do not exist. On a kernel whose sys_fopen still reports a failed
   open as a valid fd, the first such probe makes the next op hit
   ext4_assert(file && file->mp) and panics the whole kernel (fixed in
   syscall/fops.cpp �?this guard keeps the client alive on unfixed kernels,
   and is a no-op once the kernel is rebuilt). */
/* ---- in-RAM IWAD mirror (OPTIONAL fallback) ---------------------------
   The kernel's read path is fixed (fd_t::offset accounting), so DOOM reads
   lumps straight from the file on demand �?which is what the engine is
   designed for: W_ReadLump() pulls one lump at a time and the zone cache
   keeps the hot ones, while the kernel file cache serves repeated hits.
   The mirror is only a safety net for a kernel whose seek+read is broken
   again; enable it with -DSKY_WAD_MIRROR_FALLBACK=1. */
#ifndef SKY_WAD_MIRROR_FALLBACK
#define SKY_WAD_MIRROR_FALLBACK 0
#endif
static unsigned char* g_wad      = 0;
static long           g_wad_len  = 0;
static long           g_wad_pos  = 0;
#define SKY_WAD_H ((void*)1)

static int sky_is_mirror(void* h) { return g_wad != 0 && h == SKY_WAD_H; }

static int sky_wad_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    long sz = (long)fsize(f);
    if (sz <= 0) { fclose(f); return 0; }
    unsigned char* buf = (unsigned char*)malloc((size_t)sz);
    if (!buf) { fclose(f); return 0; }
    /* ONE read of the whole file: on Skyline, a sequence of small reads is
       not reliable (see sky_seek_ok �?the VFS can fall back to offset 0),
       but a single whole-file read from offset 0 is exactly how the 31 MB
       TTF font is loaded and it works. */
    fseek(f, 0, SEEK_SET);
    long got = (long)fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != sz) { free(buf); return 0; }
    if (!(buf[0] == 'I' && buf[1] == 'W' && buf[2] == 'A' && buf[3] == 'D')) {
        free(buf);
        return 0;                       /* read back garbage, refuse */
    }
    g_wad = buf; g_wad_len = sz; g_wad_pos = 0;
    return 1;
}

/* Does a seek to the lump directory actually return directory bytes?
   Decisive test: read 16 bytes at offset 0 and 16 bytes at the directory
   offset. If they are byte-identical, the file position never moved �?i.e.
   lseek is a no-op and every read is served from the start of the file.
   (Do NOT just test "the name field is non-zero": the IWAD header happens to
   have non-zero bytes at those positions and fools that check.) */
static int sky_seek_ok(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[12], a[16], b[16];
    if ((long)fread(hdr, 1, 12, f) != 12) { fclose(f); return 0; }
    uint32_t ofs = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                   ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    if (ofs == 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    if ((long)fread(a, 1, 16, f) != 16) { fclose(f); return 0; }
    if (fseek(f, (long)ofs, SEEK_SET) != 0) { fclose(f); return 0; }
    if ((long)fread(b, 1, 16, f) != 16) { fclose(f); return 0; }
    fclose(f);
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 1;     /* different data -> the seek worked */
    return 0;                           /* identical -> seek is ignored */
}

static void* sky_open(const char* fn, const char* mode) {
    if (!fn || !mode) return 0;
    if (mode[0] == 'w' || mode[0] == 'a') return 0;
    if (g_wad && sky_ends_with(fn, "doom.wad")) { g_wad_pos = 0; return SKY_WAD_H; }
    if (!sky_ends_with(fn, "doom.wad") && !sky_ends_with(fn, "doom1.wad"))
        return 0;                       /* refuse the probe: file is absent */
    return (void*)fopen(fn, "rb");
}
static void sky_close(void* h) { if (h && !sky_is_mirror(h)) fclose((FILE*)h); }

/* Lumps are pulled one at a time (W_ReadLump) and the zone cache keeps the
   hot ones, so requests are small; the loop is only a guard against a short
   read. */
#define SKY_READ_CHUNK 8192

static int  sky_seek(void* h, int offset, doom_seek_t origin);

static int  sky_read(void* h, void* buf, int count) {
    /* Read in small chunks and retry: DOOM asks for the whole lump directory
       (numlumps * 16 B, ~35 KB for doom.wad) in a single fread, and a short
       or failed large read silently leaves the lump table zeroed �?which
       surfaces much later as "W_GetNumForName, PNAMES not found!". Chunking
       keeps every request inside what the VFS read path handles. */
    if (sky_is_mirror(h)) {                     /* serve from the RAM mirror */
        if (count <= 0) return 0;
        long avail = g_wad_len - g_wad_pos;
        if (avail <= 0) return 0;
        int n = (count < avail) ? count : (int)avail;
        memcpy(buf, g_wad + g_wad_pos, (size_t)n);
        g_wad_pos += n;
        return n;
    }
    if (!h || count <= 0) return 0;
    size_t got = 0;
    int zero_retries = 0;
    while (got < (size_t)count) {
        size_t want = (size_t)count - got;
        if (want > SKY_READ_CHUNK) want = SKY_READ_CHUNK;
        size_t n = fread((char*)buf + got, 1, want, (FILE*)h);
        if (n == 0) {
            /* 内存紧张时内核的文件读可能瞬时返�?0 (中转缓冲或块缓存分配失败)�?               直接 break 会让整次 W_ReadLump 变成"只读了一�?, DOOM 随即
               I_Error 退�?—�?实测 64/72/80 MiB 下就是这样起的�?               先原地位重试; 若仍�?0, **重新 seek 回当前位置再�?* —�?               内核侧的 FD 偏移和底层文件位置可能已经不同步�?*/
            if (++zero_retries <= 32) {
                for (volatile int spin = 0; spin < 20000; spin++) { }
                if ((zero_retries & 7) == 0) {
                    sky_seek(h, (int)got, DOOM_SEEK_SET);
                }
                continue;
            }
            break;
        }
        zero_retries = 0;
        got += n;
    }
    return (int)got;
}
static int  sky_write(void* h, const void* buf, int count) {
    (void)h; (void)buf;
    return count;                     /* sink: no write support in libc yet */
}
static int  sky_seek(void* h, int offset, doom_seek_t origin) {
    if (sky_is_mirror(h)) {
        long base = (origin == DOOM_SEEK_CUR) ? g_wad_pos
                  : (origin == DOOM_SEEK_END) ? g_wad_len : 0;
        long np = base + (long)offset;
        if (np < 0) np = 0;
        if (np > g_wad_len) np = g_wad_len;
        g_wad_pos = np;
        return 0;
    }
    if (!h) return -1;
    return fseek((FILE*)h, (long)offset, (int)origin);
}
static int  sky_tell(void* h) {
    if (sky_is_mirror(h)) return (int)g_wad_pos;
    return h ? (int)ftell((FILE*)h) : -1;
}
static int  sky_eof(void* h) {
    if (sky_is_mirror(h)) return g_wad_pos >= g_wad_len ? 1 : 0;
    if (!h) return 1;
    long pos = ftell((FILE*)h);
    if (pos < 0) return 1;
    return pos >= (long)fsize((FILE*)h) ? 1 : 0;
}

/* ---- clock -------------------------------------------------------------
   I_GetTime() needs (sec, usec); syscall 17 only gives RTC whole seconds
   and a pure TSC clock drifts (host TSC rates are not invariant), so the
   clock is re-anchored on every observed RTC second boundary:

     * whole seconds come straight from the RTC  -> no cumulative drift
     * the fraction comes from the TSC, and the TSC-per-microsecond rate is
       re-measured across each real second      -> self-recalibrating

   The RTC is only probed every ~10 ms (once g_q10 is known) to keep the
   CMOS read cost out of the frame loop. */
static uint64_t g_q10         = 0;   /* TSC ticks per microsecond, Q10   */
static uint64_t g_last_sec    = 0;   /* last RTC second observed         */
static uint64_t g_tsc_at_sec  = 0;   /* TSC when that second was seen    */
static uint64_t g_last_probe  = 0;   /* TSC of the last RTC probe        */

#define SKYCLOCK_PROBE_US 10000      /* re-anchor granularity: 10 ms     */

static void sky_gettime(int* sec, int* usec) {
    /* 用内核的毫秒单调时钟 (SYSCALL_MONOTONIC_MS)�?       原来的实现是"RTC �?+ TSC 补小�?, 需要自己用 TSC 相对 RTC 秒做校准;
       实测该校准整体偏大约 1.8 �?—�?于是 DOOM �?35 Hz 节拍实际只有�?       19 Hz, 整个游戏慢放, 看起来就�?�?。内核直接给毫秒后就不需要校准了�?*/
    uint64_t ms = syscall(SYSCALL_MONOTONIC_MS, 0, 0, 0, 0, 0, 0);
    *sec  = (int)(ms / 1000u);
    *usec = (int)((ms % 1000u) * 1000u);
}

/* ---- exit / getenv ----------------------------------------------------- */
static void sky_exit(int code) { sys_exit((uint64_t)(code < 0 ? 1u : (uint64_t)code)); }

static char* sky_getenv(const char* var) {
    if (!var) return 0;
    if (strcmp(var, "DOOMWADDIR") == 0) return "/mp";
    if (strcmp(var, "HOME") == 0)       return "/mp";   /* I_Error if NULL */
    return 0;
}

/* ---- zone sizing (low-memory policy) -----------------------------------
   DOOM's zone is the dominant fixed allocation: upstream defaults to 12 MiB
   on 64-bit (mb_used = 6 * sizeof(void*)/4). On a 64 MiB QEMU machine the
   kernel reports ~56 MiB total, and desktop+console+notepad+settings leave
   only a few MiB for DOOM -- so scale the zone by machine RAM tier and log
   the decision for the QEMU serial gate. Ask the kernel's SysInfo page (the
   same source desktop's wm_total_mem_mb() uses). Must be called before
   doom_init(): Z_Init() reads mb_used via I_ZoneBase() once. */
static int sky_pick_zone_mb(uint64_t* total_mb_out, uint64_t* free_mb_out) {
    uint64_t total_mb = 0, free_mb = 0;
    SysInfo* si = (SysInfo*)sys_sysinfo(0);
    if ((int64_t)si > 0) {
        total_mb = si->mem_total >> 20;
        free_mb  = si->mem_free >> 20;
        sys_sysinfo((uint64_t)si);      /* hand the mapping back to the kernel */
    }
    int mb = 12;                        /* upstream default (>=192 MiB host) */
    if (total_mb && total_mb <= 80)       mb = 8; /* DOOM_TINY+trim: 8 MiB verified at -m64M (2x PASS) */
    else if (total_mb && total_mb <= 128) mb = 10;
    else if (total_mb && total_mb <= 192) mb = 12;
    /* NOTE: no free-RAM clamp here -- the zone size must be deterministic per
       machine tier for A/B testing (Lead's 64M measurements: 8/9 MiB
       failed during R_Init on the older kernel, 10 MiB is the smallest
       stable tier; the 12 MiB upstream default runs but triggers more
       PMM OOM). */
    if (total_mb_out) *total_mb_out = total_mb;
    if (free_mb_out)  *free_mb_out  = free_mb;
    return mb;
}

/* ---- keyboard ----------------------------------------------------------
   The ring carries ASCII (plus a few 0x100+ specials) and a modifier
   snapshot. DOOM wants lowercase letters, 0xac..0xaf arrows and separate
   down/up edges for ctrl/shift/alt, so modifiers are synthesised from the
   per-event snapshot. */
static uint16_t sky_map_key(uint16_t k) {
    if (k >= 'A' && k <= 'Z') return (uint16_t)(k - 'A' + 'a');
    if (k == '\n')  return (uint16_t)DOOM_KEY_ENTER;
    if (k == 0x08)  return (uint16_t)DOOM_KEY_BACKSPACE;
    if (k == '\t')  return (uint16_t)DOOM_KEY_TAB;
    if (k == 0x1b)  return (uint16_t)DOOM_KEY_ESCAPE;
    if (k < 0x100)  return k;                 /* digits, space, ',' '.' ... */
    switch (k) {
    case KBD_KEY_LEFT:  return (uint16_t)DOOM_KEY_LEFT_ARROW;
    case KBD_KEY_RIGHT: return (uint16_t)DOOM_KEY_RIGHT_ARROW;
    case KBD_KEY_UP:    return (uint16_t)DOOM_KEY_UP_ARROW;
    case KBD_KEY_DOWN:  return (uint16_t)DOOM_KEY_DOWN_ARROW;
    default:            return 0;             /* unmapped: ignore */
    }
}

static void sky_mods_update(uint8_t mods, uint8_t* last) {
    static const struct { uint8_t bit; doom_key_t key; } tbl[] = {
        { (uint8_t)(KBD_MOD_LCTRL  | KBD_MOD_RCTRL),  DOOM_KEY_CTRL  },
        { (uint8_t)(KBD_MOD_LSHIFT | KBD_MOD_RSHIFT), DOOM_KEY_SHIFT },
        { (uint8_t)(KBD_MOD_LALT   | KBD_MOD_RALT),   DOOM_KEY_ALT   },
    };
    for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
        int now = (mods & tbl[i].bit) != 0;
        int was = (*last & tbl[i].bit) != 0;
        if (now == was) continue;
        if (now) doom_key_down(tbl[i].key);
        else     doom_key_up(tbl[i].key);
    }
    *last = mods;
}

/* ---- mouse -------------------------------------------------------------
   The kernel publishes a shared absolute-position page; DOOM wants relative
   motion and button edges, so we diff consecutive samples and synthesise
   down/up for each button. Only fed while this window holds focus, so
   dragging another window never spins the player around. */
static ps2_mouse_state_t* g_ms = 0;
static int32_t g_mx = 0, g_my = 0;
static uint8_t g_mbtn = 0;

static void sky_mouse_pump(int focused) {
    if (!g_ms) return;
    ps2_mouse_state_t m = *g_ms;              /* one coherent snapshot */

    if (focused) {
        int32_t dx = m.x - g_mx, dy = m.y - g_my;
        if (dx || dy) doom_mouse_move((int)dx, (int)dy);
    }
    /* always track, so regaining focus does not produce one huge jump */
    g_mx = m.x;
    g_my = m.y;

    if (!focused) { g_mbtn = 0; return; }
    static const doom_button_t btn[3] = {
        DOOM_LEFT_BUTTON, DOOM_RIGHT_BUTTON, DOOM_MIDDLE_BUTTON
    };
    uint8_t now = (uint8_t)((m.left   ? 1u : 0u) |
                            (m.right  ? 2u : 0u) |
                            (m.middle ? 4u : 0u));
    for (int i = 0; i < 3; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if ((now & bit) == (g_mbtn & bit)) continue;
        if (now & bit) doom_button_down(btn[i]);
        else           doom_button_up(btn[i]);
    }
    g_mbtn = now;
}

/* ---- presentation ------------------------------------------------------
   doom_get_framebuffer(4) is 320x200 RGBA; the shared surface is ARGB
   (0xAARRGGBB, see RGB() in graphic/fb.h). Nearest-neighbour upscale.

   2026-11 "DOOM window fix": the WM derives the content height as
       contentH = bodyH - SKYWIN_TITLE_H - SkyWinRadius()
   so it depends on the *runtime theme radius* (the default desktop.lua sets
   corner_radius=16, turning the 640x440 body into a 640x392 content area;
   the old "2x if ch>=400 else 1x" rule then fell back to 1x and left a big
   black half/quarter of the window). We now always FILL the content area:
   exact 2x when it is 640x400, otherwise a precomputed nearest-neighbour map
   (x always 2x, y scaled by ch/200 -- at most ~2% vertical squash for the
   radius range 0..20). No black borders and no dependency on theme radius. */
#define BLIT_MAX_W 1024u
#define BLIT_MAX_H 1024u
static uint16_t g_colmap[BLIT_MAX_W];   /* dest x -> source x */
static uint16_t g_rowmap[BLIT_MAX_H];   /* dest y -> source y */
static uint32_t g_mapW = 0, g_mapH = 0;

static void blit_setup(uint32_t cw, uint32_t ch) {
    if (cw > BLIT_MAX_W) cw = BLIT_MAX_W;
    if (ch > BLIT_MAX_H) ch = BLIT_MAX_H;
    g_mapW = cw;
    g_mapH = ch;
    for (uint32_t x = 0; x < cw; x++)
        g_colmap[x] = (uint16_t)((uint64_t)x * DOOM_SCR_W / cw);
    for (uint32_t y = 0; y < ch; y++)
        g_rowmap[y] = (uint16_t)((uint64_t)y * DOOM_SCR_H / ch);
}

#if DOOM_TINY
/* Compact build: source is the engine's 8-bit palette-index screen. */
static void blit_frame(uint32_t* dst, uint32_t pitch,
                       const unsigned char* src) {
    if (!dst || !src || !g_mapW || !g_mapH) return;
    for (uint32_t y = 0; y < g_mapH; y++) {
        const unsigned char* srow =
            src + (uint64_t)g_rowmap[y] * DOOM_SCR_W;
        uint32_t* drow = dst + (uint64_t)y * pitch;
        for (uint32_t x = 0; x < g_mapW; x++)
            drow[x] = g_argb_lut[srow[g_colmap[x]]];
    }
}
#else
/* Normal build: source is the engine's 320x200 RGBA buffer. */
static void blit_frame(uint32_t* dst, uint32_t pitch,
                       const unsigned char* src) {
    if (!dst || !src || !g_mapW || !g_mapH) return;
    for (uint32_t y = 0; y < g_mapH; y++) {
        const unsigned char* srow =
            src + (uint64_t)g_rowmap[y] * DOOM_SCR_W * 4u;
        uint32_t* drow = dst + (uint64_t)y * pitch;
        for (uint32_t x = 0; x < g_mapW; x++) {
            const unsigned char* p = srow + (uint64_t)g_colmap[x] * 4u;
            drow[x] = 0xFF000000u | ((uint32_t)p[0] << 16) |
                      ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        }
    }
}
#endif

/* Minimal serial formatting (no printf dependency: the shim must build
   against both Skyline's libc and the plain host libc used by hosttest). */
static void sky_dbg_hex(const char* tag, const unsigned char* p, int n) {
    static const char HX[] = "0123456789abcdef";
    char line[220];
    int o = 0;
    for (const char* s = "[doom] "; *s; s++) line[o++] = *s;
    for (const char* s = tag; *s; s++) line[o++] = *s;
    line[o++] = ':'; line[o++] = ' ';
    for (int i = 0; i < n && o < 190; i++) {
        line[o++] = HX[p[i] >> 4];
        line[o++] = HX[p[i] & 15];
        line[o++] = ' ';
    }
    line[o++] = '\n'; line[o] = '\0';
    sky_dbg(line);
}

static void sky_dbg_num(const char* tag, unsigned long v) {
    char line[160], tmp[24];
    int o = 0, t = 0;
    for (const char* s = "[doom] "; *s; s++) line[o++] = *s;
    for (const char* s = tag; *s; s++) line[o++] = *s;
    line[o++] = '=';
    if (!v) tmp[t++] = '0';
    while (v) { tmp[t++] = (char)('0' + (v % 10)); v /= 10; }
    while (t) line[o++] = tmp[--t];
    line[o++] = '\n'; line[o] = '\0';
    sky_dbg(line);
}

int main(void) {
    volatile uint64_t* q = (volatile uint64_t*)SKYWIN_PROTO_PAGE_VA;
    uint64_t contentVA = q[SKYWIN_PROTO_CONTENT_VA];
    uint32_t pitch     = (uint32_t)q[SKYWIN_PROTO_PITCH];
    uint32_t cw        = (uint32_t)q[SKYWIN_PROTO_CONTENT_W];
    uint32_t ch        = (uint32_t)q[SKYWIN_PROTO_CONTENT_H];
    if (!contentVA || !pitch) return 1;

    sky_dbg("[doom] client start\n");

    doom_set_print(sky_dbg);
    doom_set_malloc(sky_malloc, sky_free);
    doom_set_file_io(sky_open, sky_close, sky_read, sky_write,
                     sky_seek, sky_tell, sky_eof);
    doom_set_gettime(sky_gettime);
    doom_set_exit(sky_exit);
    doom_set_getenv(sky_getenv);

    /* modern bindings (PureDOOM defaults are arrows + ',' / '.' strafe) */
    doom_set_default_int("key_up",          DOOM_KEY_W);
    doom_set_default_int("key_down",        DOOM_KEY_S);
    doom_set_default_int("key_strafeleft",  DOOM_KEY_A);
    doom_set_default_int("key_straferight", DOOM_KEY_D);
    doom_set_default_int("key_use",         DOOM_KEY_E);
    doom_set_default_int("mouse_move",      0);

    char* argv[] = { (char*)"doom.elf" };
    if (SKY_WAD_MIRROR_FALLBACK && !sky_seek_ok("/mp/doom.wad")) {
        sky_dbg("[doom] VFS seek+read unusable -> "
                "mirroring the IWAD in RAM\n");
        if (sky_wad_load("/mp/doom.wad")) {
            sky_dbg("[doom] IWAD mirrored in RAM, bytes:\n");
            sky_dbg_num("mirror", (unsigned long)g_wad_len);
        } else {
            sky_dbg("[doom] WARN: IWAD mirror failed, falling back to VFS\n");
        }
    } else {
    }
    /* Zone 大小按机器内存档位缩放（64MiB 目标�?-> 4MiB；见 sky_pick_zone_mb）�?       DOOM 默认 zone = 6*(sizeof(void*)/4) = 12MiB(64-bit)，在 55MiB 可用内存�?       会把桌面/其它应用挤进 OOM。必须在 doom_init() 之前设置：Z_Init() 只经
       I_ZoneBase() 读一�?mb_used�?*/
    {
        uint64_t total_mb = 0, free_mb = 0;
        int zone_mb = sky_pick_zone_mb(&total_mb, &free_mb);
        doom_set_zone_mb(zone_mb);
        sky_dbg_num("zone_mb", (unsigned long)zone_mb);
        sky_dbg_num("mem_total_mb", (unsigned long)total_mb);
        sky_dbg_num("mem_free_mb", (unsigned long)free_mb);
    }
    /* WAD size evidence for the serial gate. The trimmed WAD must be
       installed as /mp/doom.wad (or /mp/doom1.wad): those are the names the
       engine probes under DOOMWADDIR=/mp. doom_trim.wad is logged when both
       files exist. */
    {
        FILE* wf = fopen("/mp/doom.wad", "rb");
        long plain = wf ? (long)fsize(wf) : -1;
        if (wf) fclose(wf);
        FILE* tf = fopen("/mp/doom_trim.wad", "rb");
        long trim = tf ? (long)fsize(tf) : -1;
        if (tf) fclose(tf);
        if (plain > 0) {
            char line[128];
            int n = snprintf(line, sizeof(line),
                             "[doom] wad bytes=%ld -> %ld (trim file=%ld)\n",
                             plain, trim > 0 ? trim : plain, trim);
            if (n > 0)
                syscall(SYSCALL_DBGSOUT, (uint64_t)line, (uint64_t)n, 0, 0, 0, 0);
        }
    }
    doom_init(1, argv, DOOM_FLAG_HIDE_MOUSE_OPTIONS |
                       DOOM_FLAG_HIDE_SOUND_OPTIONS |
                       DOOM_FLAG_HIDE_MUSIC_OPTIONS);
    sky_dbg("[doom] init done\n");
#if DOOM_TINY
    sky_dbg("[doom] compact=1 screens_kb=64 final_rgba_kb=0 screen_copy_kb=0\n");
#else
    sky_dbg("[doom] compact=0 screens_kb=256 final_rgba_kb=256 screen_copy_kb=64\n");
#endif
    {
        SysInfo* si = (SysInfo*)sys_sysinfo(0);
        if ((int64_t)si > 0) {
            unsigned long free_mb = (unsigned long)(si->mem_free >> 20);
            sys_sysinfo((uint64_t)si);
            sky_dbg_num("mem_free_mb_post_init", free_mb);
        }
    }

    uint64_t kbdVA = syscall(SYSCALL_DEV_MMAP, (uint64_t)KBD_DEV_TYPE,
                             0, 0, 0, 0, 0);
    KbdShared* kbd = (KbdShared*)kbdVA;
    int slot = kbd ? kbd_reader_register(kbd, (int32_t)sys_getpid()) : -1;
    uint64_t cursor = (slot >= 0) ? kbd_reader_pos(kbd, slot) : 0;
    uint8_t last_mods = 0;

    uint64_t msVA = syscall(SYSCALL_DEV_MMAP, (uint64_t)DEV_TYPE_PS2_MOUSE,
                            0, 0, 0, 0, 0);
    if (msVA) {
        g_ms = (ps2_mouse_state_t*)msVA;
        g_mx = g_ms->x; g_my = g_ms->y;      /* no jump on the first sample */
    }

    uint32_t* surf = (uint32_t*)contentVA;
    /* 内容区改为“始终填满”（�?blit_frame 注释）：runtime theme radius 会把
       contentH 变成 392 等非 400 值，旧的 1x 回退会留大片黑边�?*/
    blit_setup(cw, ch);
    sky_dbg_num("content_w", (unsigned long)cw);
    sky_dbg_num("content_h", (unsigned long)ch);
    sky_dbg_num("exact_2x", (cw == 640u && ch == 400u) ? 1u : 0u);

    for (;;) {
        int focused = 1;
        if (kbd) {
            uint64_t h = __atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE);
            cursor = kbd_reader_resync(kbd, cursor);
            int allowed = (slot < 0) || kbd_focus_allows(kbd, slot);
            focused = allowed;
            while (cursor < h) {
                KbdEvent e = kbd->ring[cursor & (KBD_RING_CAP - 1u)];
                if (allowed) {
                    sky_mods_update(e.mods, &last_mods);
                    uint16_t dk = sky_map_key(e.key);
                    if (dk) {
                        if (e.action == KBD_ACTION_DOWN) doom_key_down((doom_key_t)dk);
                        else                             doom_key_up((doom_key_t)dk);
                    }
                }
                cursor++;
            }
            if (slot >= 0) kbd_reader_setpos(kbd, slot, cursor);
        }

        sky_mouse_pump(focused);

        /* 帧率限制 (DOOM 原生 35 Hz)�?           没有它主循环会跑�?200+ fps: 每一轮都递增 OUT_SEQ, 等于要求桌面
           每秒重组两百多次, 合成器直接被这一路请求占�?—�?表现就是"DOOM
           跑着跑着整个桌面卡住/卡顿"。放在输入泵之后, 输入延迟不受影响�?*/
        {
            static int64_t next_us;
            int gs = 0, gu = 0;
            sky_gettime(&gs, &gu);
            int64_t now_us = (int64_t)gs * 1000000 + (int64_t)gu;
            if (next_us && now_us < next_us) {
                /* **有界等待**: 时钟一旦停止前�?(跨核 TSC 偏差 / 时钟冻结),
                   `while (now < next) yield;` 会永远等下去 —�?实测表现�?                   "DOOM 跑一会就卡死": 心跳停止、进程还在、内核无恙�?                   这里给等待加硬上�? 最多让�?DOOM_WAIT_MAX 次就放弃本帧�?                   节拍限制继续渲染。宁可掉几帧, 绝不永久卡住�?*/
                static unsigned long wait_spins;
                if (next_us - now_us > 200000) {
                    next_us = now_us;              /* 时钟异常: 重置节拍 */
                } else if (++wait_spins < 200000ul) {
                    sys_yield();
                    continue;
                } else {
                    wait_spins = 0;                /* 放弃本帧等待 */
                    next_us = now_us;
                }
            }
            next_us = now_us + (1000000 / 35);
        }

        doom_update();
#if DOOM_TINY
        sky_build_palette_lut();
        blit_frame(surf, pitch, doom_get_framebuffer(1));
#else
        blit_frame(surf, pitch, doom_get_framebuffer(4));
#endif
        q[SKYWIN_PROTO_OUT_SEQ]++;     /* new frame: ask the WM to recompose */

        /* 帧心�? �?128 帧往串口打一行�?           用�?—�?用户�?DOOM 运行到一半卡�?�? 看这行就能定�?
             * 心跳停了            -> DOOM 自己的主循环卡住 (doom_update/blit_frame)
             * 心跳继续但画面不�? -> 桌面合成/共享表面那边的问�? �?DOOM 无关
           没有这个判别, 只能靠猜�?*/
        {
            static uint64_t frames;
            if ((++frames & 127u) == 0) {
                char hb[80];
                int hl = snprintf(hb, sizeof(hb), "[doom] f=%lu ms=%lu seq=%lu\n",
                                  (unsigned long)frames,
                                  (unsigned long)syscall(SYSCALL_MONOTONIC_MS, 0, 0, 0, 0, 0, 0),
                                  (unsigned long)q[SKYWIN_PROTO_OUT_SEQ]);
                if (hl > 0) syscall(SYSCALL_DBGSOUT, (uint64_t)hb, (uint64_t)hl, 0, 0, 0, 0);
            }
        }
        sys_yield();
    }
    return 0;
}
