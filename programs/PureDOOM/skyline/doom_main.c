//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
//
// doom_main.c — PureDOOM host shim for SkylineSystem.
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
   syscall/fops.cpp — this guard keeps the client alive on unfixed kernels,
   and is a no-op once the kernel is rebuilt). */
/* ---- in-RAM IWAD mirror (fallback) ------------------------------------
   If the VFS cannot service "seek to a far offset then read" (which is how
   DOOM reads the lump directory), we mirror the whole IWAD into RAM once at
   startup with plain sequential reads from offset 0, and serve every later
   open/read/seek/tell from the mirror. Costs ~11 MB and removes the VFS
   offset semantics from the game's path entirely. */
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
       not reliable (see sky_seek_ok — the VFS can fall back to offset 0),
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
   offset. If they are byte-identical, the file position never moved — i.e.
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
#define SKY_READ_CHUNK 1024

static int  sky_read(void* h, void* buf, int count) {
    /* Read in small chunks and retry: DOOM asks for the whole lump directory
       (numlumps * 16 B, ~35 KB for doom.wad) in a single fread, and a short
       or failed large read silently leaves the lump table zeroed — which
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
    while (got < (size_t)count) {
        size_t want = (size_t)count - got;
        if (want > SKY_READ_CHUNK) want = SKY_READ_CHUNK;
        size_t n = fread((char*)buf + got, 1, want, (FILE*)h);
        if (n == 0) break;
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
    uint64_t now = rdtsc();

    if (!g_q10 || now - g_last_probe >
                      (g_q10 * (uint64_t)SKYCLOCK_PROBE_US) >> 10) {
        g_last_probe = now;
        uint64_t s = syscall(SYSCALL_TIME, 0, 0, 0, 0, 0, 0);
        if (s != g_last_sec) {
            if (g_last_sec && s > g_last_sec && now > g_tsc_at_sec) {
                uint64_t new_q10 = ((now - g_tsc_at_sec) << 10) /
                                   ((s - g_last_sec) * 1000000ull);
                if (new_q10) g_q10 = new_q10;   /* measured over a real second */
            }
            g_last_sec   = s;
            g_tsc_at_sec = now;
        }
    }

    uint64_t frac = g_q10 ? (((now - g_tsc_at_sec) << 10) / g_q10) : 0;
    if (frac > 999999ull) frac = 999999ull;   /* hold until the next second */
    *sec  = (int)g_last_sec;
    *usec = (int)frac;
}

/* ---- exit / getenv ----------------------------------------------------- */
static void sky_exit(int code) { sys_exit((uint64_t)(code < 0 ? 1u : (uint64_t)code)); }

static char* sky_getenv(const char* var) {
    if (!var) return 0;
    if (strcmp(var, "DOOMWADDIR") == 0) return "/mp";
    if (strcmp(var, "HOME") == 0)       return "/mp";   /* I_Error if NULL */
    return 0;
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
   (0xAARRGGBB, see RGB() in graphic/fb.h). Nearest-neighbour upscale. */
static void blit_frame(uint32_t* dst, uint32_t pitch, uint32_t cw, uint32_t ch,
                       const unsigned char* src, uint32_t scale) {
    uint32_t dw = DOOM_SCR_W * scale, dh = DOOM_SCR_H * scale;
    if (dw > cw) dw = cw;
    if (dh > ch) dh = ch;

    if (scale == 2) {
        for (uint32_t y = 0; y < dh; y++) {
            const unsigned char* srow = src + (uint64_t)(y >> 1) * DOOM_SCR_W * 4u;
            uint32_t* drow = dst + (uint64_t)y * pitch;
            for (uint32_t x = 0; x < DOOM_SCR_W; x++) {
                const unsigned char* p = srow + (uint64_t)x * 4u;
                uint32_t px = 0xFF000000u | ((uint32_t)p[0] << 16) |
                              ((uint32_t)p[1] << 8) | (uint32_t)p[2];
                drow[2u * x]       = px;
                drow[2u * x + 1u]  = px;
            }
        }
        return;
    }
    for (uint32_t y = 0; y < dh; y++) {
        const unsigned char* srow = src + (uint64_t)(y / scale) * DOOM_SCR_W * 4u;
        uint32_t* drow = dst + (uint64_t)y * pitch;
        for (uint32_t x = 0; x < dw; x++) {
            const unsigned char* p = srow + (uint64_t)(x / scale) * 4u;
            drow[x] = 0xFF000000u | ((uint32_t)p[0] << 16) |
                      ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        }
    }
}

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

/* IWAD sanity probe: header reads fine but the lump directory did not, so
   dump raw bytes at several offsets to tell "the image data is zero here"
   apart from "fseek does not move the file position". */
static void sky_wad_probe(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        sky_dbg("[doom] probe: cannot open IWAD\n");
        return;
    }
    sky_dbg_num("size", (unsigned long)fsize(f));

    unsigned char hdr[12];
    long got = (long)fread(hdr, 1, 12, f);
    uint32_t numlumps = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) |
                        ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    uint32_t tableofs = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                        ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    sky_dbg_hex("magic", hdr, 4);              /* 49 57 41 44 = IWAD */
    sky_dbg_num("hdr_bytes", (unsigned long)got);
    sky_dbg_num("numlumps", numlumps);
    sky_dbg_num("tableofs", tableofs);

    unsigned char b[16];
    if (fseek(f, 0, SEEK_SET) == 0) {
        if ((long)fread(b, 1, 16, f) == 16) sky_dbg_hex("off0", b, 16);
    }
    if (fseek(f, (long)tableofs, SEEK_SET) == 0) {
        if ((long)fread(b, 1, 16, f) == 16)
            sky_dbg_hex("offtable", b, 16);    /* last 8 bytes = lump name */
    } else {
        sky_dbg("[doom] probe: seek to directory FAILED\n");
    }
    if (fseek(f, 0, SEEK_SET) == 0) {
        if ((long)fread(b, 1, 16, f) == 16) sky_dbg_hex("off0-again", b, 16);
    }
    sky_dbg("[doom] probe: if offtable == off0 -> the seek is being ignored\n");
    fclose(f);
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
    sky_wad_probe("/mp/doom.wad");
    if (!sky_seek_ok("/mp/doom.wad")) {
        sky_dbg("[doom] VFS cannot seek+read the lump directory -> "
                "mirroring the IWAD in RAM\n");
        if (sky_wad_load("/mp/doom.wad")) {
            sky_dbg("[doom] IWAD mirrored in RAM, bytes:\n");
            sky_dbg_num("mirror", (unsigned long)g_wad_len);
        } else {
            sky_dbg("[doom] WARN: IWAD mirror failed, falling back to VFS\n");
        }
    } else {
        sky_dbg("[doom] VFS seek+read OK, using file I/O directly\n");
    }
    doom_init(1, argv, DOOM_FLAG_HIDE_MOUSE_OPTIONS |
                       DOOM_FLAG_HIDE_SOUND_OPTIONS |
                       DOOM_FLAG_HIDE_MUSIC_OPTIONS);
    sky_dbg("[doom] init done\n");

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
    uint32_t scale = (cw >= DOOM_SCR_W * 2u && ch >= DOOM_SCR_H * 2u) ? 2u : 1u;

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

        doom_update();
        blit_frame(surf, pitch, cw, ch, doom_get_framebuffer(4), scale);
        q[SKYWIN_PROTO_OUT_SEQ]++;     /* new frame: ask the WM to recompose */
        sys_yield();
    }
    return 0;
}
