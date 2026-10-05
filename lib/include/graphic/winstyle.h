//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// winstyle.h — shared geometry/protocol for the userspace console window.
//
// The console is a FIXED-SIZE, centered, modern (Win11/Fluent style) window:
// one cool-dark rounded surface (anti-aliased corner radius), a flat caption
// with a close cross, a near-black terminal paper, and a soft drop shadow.
//
// The ARGB window SURFACE is deliberately larger than the visible body: a
// symmetric SKYWIN_SHADOW margin around the body holds the soft shadow and the
// corner anti-aliasing, so rounded corners and shadow correctly composite
// over the wallpaper (per-pixel alpha). The body itself lives at
// (SKYWIN_SHADOW, SKYWIN_SHADOW) inside the tightly-packed surface, whose
// scanline pitch equals SKYWIN_SURF_W.
//
// Both desktop/TLoad (which allocates + shares the surface AND paints the
// window decoration) and the hw2 client (which only runs printf into the
// content area) include this header, so the geometry can never drift between
// the two sides.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- visual window body: the rounded rectangle the user sees ----------- */
#define SKYWIN_W          620u   /* body width  (px)                        */
#define SKYWIN_H          400u   /* body height                             */
#define SKYWIN_RADIUS      8u   /* Win11-style corner radius               */
#define SKYWIN_TITLE_H    32u   /* modern caption/title-bar height         */
#define SKYWIN_CAPBTN_W   46u   /* width of EACH caption button (min/max/x) */
#define SKYWIN_BTN_W      SKYWIN_CAPBTN_W
/* Body-local left x of each RIGHT-aligned caption button for a body whose
   width is B (the normal body is SKYWIN_W wide; the maximized surface spans
   the whole work-area width). Right-to-left order matches Win11:
     close    [B-1*BTN, B)
     maximize [B-2*BTN, B-1*BTN)
     minimize [B-3*BTN, B-2*BTN)                                        */
#define SKYWIN_BTN_CLOSE_L(B) ((B) - 1u * SKYWIN_BTN_W)
#define SKYWIN_BTN_MAX_L(B)   ((B) - 2u * SKYWIN_BTN_W)
#define SKYWIN_BTN_MIN_L(B)   ((B) - 3u * SKYWIN_BTN_W)

/* Bottom desktop taskbar height; it hosts the minimized-window restore
   entry on the left and a clock/power tray on the right. A maximized window
   fills the work area ABOVE this bar. */
#define SKYWIN_TASKBAR_H   44u

/* Interactive resize: an invisible hit band this wide around the body edges
   lets the user drag an edge/corner to resize the window (8 directions). The
   client content is a fixed bitmap, so it stays 1:1 crisp in the top-left and
   the body is clamped to these minimums / the work-area maximums. */
#define SKYWIN_RESIZE_BORDER 8u
#define SKYWIN_MIN_W       340u
#define SKYWIN_MIN_H       220u

/* The ARGB surface is larger than the body: a symmetric margin around it    *
 * holds the soft drop shadow and the anti-aliased corner falloff. The body  *
 * starts at (SKYWIN_SHADOW, SKYWIN_SHADOW) inside the tightly-packed surface.*/
#define SKYWIN_SHADOW      26u  /* shadow / anti-alias margin around body   */
#define SKYWIN_SURF_W     (SKYWIN_W + 2u * SKYWIN_SHADOW)
#define SKYWIN_SURF_H     (SKYWIN_H + 2u * SKYWIN_SHADOW)
#define SKYWIN_BODY_X      SKYWIN_SHADOW
#define SKYWIN_BODY_Y      SKYWIN_SHADOW

/* ---- terminal content area, expressed in SURFACE coordinates ----------- */
#define SKYWIN_CONTENT_X   SKYWIN_SHADOW
#define SKYWIN_CONTENT_Y   (SKYWIN_SHADOW + SKYWIN_TITLE_H)
#define SKYWIN_CONTENT_W   (SKYWIN_W)
/* The terminal paper is a plain OPAQUE rectangle, so it stops RADIUS rows
   short of the body's bottom edge: otherwise it would paint over the rounded
   bottom corners and square them off. The WM owns those final rows and rounds
   them; both are the same black, so the join is seamless. */
#define SKYWIN_CONTENT_H   (SKYWIN_H - SKYWIN_TITLE_H - SKYWIN_RADIUS)

/* ---- fixed protocol page (client VA 0x400000), one uint64 per slot ------
 * The desktop fills the geometry slots before launch and polls the handshake
 * slots after launch.
 * [0..4] is EXACTLY what the libc flanterm console reads on first printf:
 *   [0] content-area VA in client     [1] content bytes
 *   [2] content width                [3] content height   [4] pitch
 * The pitch is the SURFACE pitch (SKYWIN_SURF_W), because each content row is
 * a window-body-wide run strided across the wider shadow-bearing surface.
 * [5..7] describe the whole window SURFACE (the desktop WM paints the chrome;
 *        kept for protocol completeness):
 *   [5] whole-surface VA in client   [6] SKYWIN_SURF_W    [7] SKYWIN_SURF_H
 * [8..9] are the client -> WM readiness handshake (written by the client):
 *   [8] CLIENT_READY = SKYWIN_READY_MAGIC once the client has finished its
 *       initial surface setup (console context up, or first self-paint done);
 *   [9] OUT_SEQ, console-only: incremented after every rendered character so
 *       the WM can wait until the initial output burst has gone quiet.
 *       Non-console apps (notepad) leave it at 0; READY alone is sufficient. */
#define SKYWIN_PROTO_CONTENT_VA   0u
#define SKYWIN_PROTO_CONTENT_SZ   1u
#define SKYWIN_PROTO_CONTENT_W    2u
#define SKYWIN_PROTO_CONTENT_H    3u
#define SKYWIN_PROTO_PITCH        4u
#define SKYWIN_PROTO_WHOLE_VA     5u
#define SKYWIN_PROTO_WIN_W        6u
#define SKYWIN_PROTO_WIN_H        7u
#define SKYWIN_PROTO_CLIENT_READY 8u
#define SKYWIN_PROTO_OUT_SEQ      9u
#define SKYWIN_PROTO_SLOTS        16u
#define SKYWIN_PROTO_PAGE_VA  0x400000UL  /* client fixed .prepad protocol page */

/* Readiness handshake magic: stored little-endian, so in memory it reads as
   the ASCII word "READY" with 'R' at the lowest byte. */
#define SKYWIN_READY_MAGIC ( \
    (uint64_t)'R'         | ((uint64_t)'E' << 8)  | \
    ((uint64_t)'A' << 16) | ((uint64_t)'D' << 24) | \
    ((uint64_t)'Y' << 32))

/* Bounded readiness wait used by the WM after launch; it replaces the old
 * fixed blind yield count. The WM polls the handshake once per sys_yield():
 *   - up to SKYWIN_STARTUP_WAIT_MAX iterations total (a hard cap, so a client
 *     that never signals can never hang the desktop during boot);
 *   - console clients (OUT_SEQ becomes non-zero) mount once OUT_SEQ has stayed
 *     unchanged for SKYWIN_STARTUP_QUIET consecutive samples (burst landed);
 *   - non-console clients (OUT_SEQ stays 0) mount SKYWIN_READY_SETTLE samples
 *     after CLIENT_READY is observed. */
#define SKYWIN_STARTUP_WAIT_MAX  2000u
#define SKYWIN_STARTUP_QUIET       32u
#define SKYWIN_READY_SETTLE         8u

/* Placement returned by TLoad to the desktop compositor. w/h describe the
   whole ARGB SURFACE (body + shadow margin); x/y is its top-left on scanout. */
typedef struct SkyWinPlacement {
    uint64_t desk_surf;   /* desktop-side alias VA of the whole surface    */
    uint64_t client_pid;  /* spawned console process (hw2) pid, for kill   */
    uint32_t w, h;        /* surface size (SKYWIN_SURF_W x SKYWIN_SURF_H)  */
    uint32_t x, y;        /* top-left position on the scanout (centered)   */
    uint64_t proto;       /* desktop-side alias of the client protocol
                             page; WM watches OUT_SEQ here to mark the
                             window dirty on client output (round 19)     */
} SkyWinPlacement;

/* ---- modern dark (Win11/Fluent) palette, 0xAARRGGBB, matches RGB() ------ *
 * One cool-dark rounded surface: a flat caption (no contrast colour bar), a *
 * near-black terminal paper, a 1px luminous hairline rim and a faint caption*
 * /paper separator. The soft drop shadow is emitted per-pixel by the WM      *
 * rasterizer (alpha over the wallpaper), not from a flat colour here.        */
#define SKYRGB_TITLE    0xFF26262Au  /* caption + window face, cool dark grey */
#define SKYRGB_BORDER   0xFF3D3D44u  /* 1px inner hairline on the rounded rim */
#define SKYRGB_SEP      0xFF2E2E33u  /* caption/paper 1px separator          */
#define SKYRGB_PAPER    0xFF000000u  /* terminal paper; matches flanterm bg  */
#define SKYRGB_INK      0xFFECECECu  /* caption glyph, near white            */
#define SKYRGB_CLOSE    0xFFD8D8DEu  /* close cross glyph                    */
#define SKYRGB_TASKBAR  0xFF1C1C20u  /* bottom taskbar strip                 */
#define SKYRGB_TBTN_IDLE 0xFF2A2A30u /* taskbar app button, window shown     */
#define SKYRGB_TBTN_ON  0xFF3A3A42u  /* taskbar app button, minimized/active */
#define SKYRGB_ACCENT   0xFF60CDFFu  /* Fluent active/accent indicator       */
#define SKYRGB_TRAY_INK 0xFFE9E9EFu  /* clock text / power glyph, soft white  */
#define SKYRGB_TB_HILITE 0xFF34343Cu /* 1px luminous hairline on bar's top    */
/* Acrylic taskbar: each bar pixel keeps this much of the wallpaper and mixes
   the rest with a cool-dark fill, so the wallpaper glows through (Win11/macOS
   translucency) instead of sitting behind a flat black strip. */
#define SKY_ACRYLIC_BASE  0xFF1B1B21u
#define SKY_ACRYLIC_KEEP  26u       /* 0..255 wallpaper fraction retained     */
/* Wall-clock display zone. QEMU's CMOS RTC defaults to UTC; shift to local
   civil time here (UTC+8, Asia/Shanghai). Change if the RTC is localtime. */
#define SKY_LOCAL_TZ_MIN  (8 * 60)
#define SKY_TRAY_MARGIN   14u       /* right inset of the clock/power tray    */

#ifdef __cplusplus
}
#endif
