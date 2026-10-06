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
// the shareware IWAD must live at /mp/doom1.wad.
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
static void* sky_open(const char* fn, const char* mode) {
    if (!fn || !mode) return 0;
    if (mode[0] == 'w' || mode[0] == 'a') return 0;
    return (void*)fopen(fn, "rb");
}
static void sky_close(void* h) { if (h) fclose((FILE*)h); }
static int  sky_read(void* h, void* buf, int count) {
    if (!h) return -1;
    return (int)fread(buf, 1, (size_t)(count > 0 ? count : 0), (FILE*)h);
}
static int  sky_write(void* h, const void* buf, int count) {
    (void)h; (void)buf;
    return count;                     /* sink: no write support in libc yet */
}
static int  sky_seek(void* h, int offset, doom_seek_t origin) {
    if (!h) return -1;
    return fseek((FILE*)h, (long)offset, (int)origin);
}
static int  sky_tell(void* h) { return h ? (int)ftell((FILE*)h) : -1; }
static int  sky_eof(void* h) {
    if (!h) return 1;
    long pos = ftell((FILE*)h);
    if (pos < 0) return 1;
    return pos >= (long)fsize((FILE*)h) ? 1 : 0;
}

/* ---- clock -------------------------------------------------------------
   I_GetTime() needs (sec, usec); syscall 17 only gives RTC whole seconds.
   We calibrate the TSC against one RTC second at startup and derive
   microseconds from rdtsc() afterwards (userspace rdtsc is unprivileged
   here; the compositor already relies on it). */
static uint64_t g_tsc_per_us_q10 = 0;   /* TSC ticks per microsecond, Q10   */
static uint64_t g_t0_tsc         = 0;
static int      g_t0_sec         = 0;

static void sky_gettime(int* sec, int* usec) {
    if (!g_tsc_per_us_q10) { *sec = 0; *usec = 0; return; }
    uint64_t us = ((rdtsc() - g_t0_tsc) << 10) / g_tsc_per_us_q10;
    *sec  = (int)(g_t0_sec + (int64_t)(us / 1000000ull));
    *usec = (int)(us % 1000000ull);
}

static void calibrate_clock(void) {
    /* The bound is expressed in TSC ticks, not iterations: an iteration
       budget of "one second" is meaningless before we know the TSC rate,
       and a stuck RTC would otherwise spin for minutes. 4e9 ticks is at
       most ~4 s on any >= 1 GHz part, and ~1.3 s at 3 GHz — by then a
       working RTC has certainly ticked. */
    const uint64_t cap = 4000000000ull;

    uint64_t s0 = syscall(SYSCALL_TIME, 0, 0, 0, 0, 0, 0);
    uint64_t t0 = rdtsc();
    uint64_t s1 = s0;
    for (uint64_t i = 0;; i++) {
        if ((i & 1023ull) == 0) {
            s1 = syscall(SYSCALL_TIME, 0, 0, 0, 0, 0, 0);
            if (s1 != s0) break;
        }
        if (rdtsc() - t0 > cap) break;
    }
    uint64_t t1 = rdtsc();

    if (s1 == s0 || t1 <= t0) {
        /* RTC did not advance: fall back to a nominal 3 GHz so the game
           still runs (paced wrong, but not frozen). */
        sky_dbg("[doom] WARN: RTC did not tick, using nominal 3GHz TSC\n");
        g_t0_sec         = 0;
        g_t0_tsc         = t1;
        g_tsc_per_us_q10 = 3000ull << 10;
        return;
    }
    g_t0_sec         = (int)s1;
    g_t0_tsc         = t1;
    g_tsc_per_us_q10 = ((t1 - t0) << 10) / 1000000ull;
    if (!g_tsc_per_us_q10) g_tsc_per_us_q10 = 1;
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

    calibrate_clock();

    /* modern bindings (PureDOOM defaults are arrows + ',' / '.' strafe) */
    doom_set_default_int("key_up",          DOOM_KEY_W);
    doom_set_default_int("key_down",        DOOM_KEY_S);
    doom_set_default_int("key_strafeleft",  DOOM_KEY_A);
    doom_set_default_int("key_straferight", DOOM_KEY_D);
    doom_set_default_int("key_use",         DOOM_KEY_E);
    doom_set_default_int("mouse_move",      0);

    char* argv[] = { (char*)"doom.elf" };
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

    uint32_t* surf = (uint32_t*)contentVA;
    uint32_t scale = (cw >= DOOM_SCR_W * 2u && ch >= DOOM_SCR_H * 2u) ? 2u : 1u;

    for (;;) {
        if (kbd) {
            uint64_t h = __atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE);
            cursor = kbd_reader_resync(kbd, cursor);
            int allowed = (slot < 0) || kbd_focus_allows(kbd, slot);
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

        doom_update();
        blit_frame(surf, pitch, cw, ch, doom_get_framebuffer(4), scale);
        q[SKYWIN_PROTO_OUT_SEQ]++;     /* new frame: ask the WM to recompose */
        sys_yield();
    }
    return 0;
}
