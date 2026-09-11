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
#define SKYWIN_CAPBTN_W   46u   /* caption (close) button area width       */

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
 * The client is a plain, portable Hello World: it never reads this page and
 * never signals any handshake. The desktop fills every slot before launch.
 * [0..4] is EXACTLY what the libc flanterm console reads on first printf:
 *   [0] content-area VA in client     [1] content bytes
 *   [2] content width                [3] content height   [4] pitch
 * The pitch is the SURFACE pitch (SKYWIN_SURF_W), because each content row is
 * a window-body-wide run strided across the wider shadow-bearing surface.
 * [5..7] describe the whole window SURFACE (the desktop WM paints the chrome;
 *        kept for protocol completeness):
 *   [5] whole-surface VA in client   [6] SKYWIN_SURF_W    [7] SKYWIN_SURF_H */
#define SKYWIN_PROTO_CONTENT_VA   0u
#define SKYWIN_PROTO_CONTENT_SZ   1u
#define SKYWIN_PROTO_CONTENT_W    2u
#define SKYWIN_PROTO_CONTENT_H    3u
#define SKYWIN_PROTO_PITCH        4u
#define SKYWIN_PROTO_WHOLE_VA     5u
#define SKYWIN_PROTO_WIN_W        6u
#define SKYWIN_PROTO_WIN_H        7u
#define SKYWIN_PROTO_SLOTS        16u
#define SKYWIN_PROTO_PAGE_VA  0x400000UL  /* client fixed .prepad protocol page */

/* After launch the WM yields this many times so the fresh client can load its
   font and emit its initial printf text before the window is mounted; the
   client itself does no synchronization. Remaining output is picked up by the
   compositor's periodic Compose. */
#define SKYWIN_STARTUP_YIELDS     256u

/* Placement returned by TLoad to the desktop compositor. w/h describe the
   whole ARGB SURFACE (body + shadow margin); x/y is its top-left on scanout. */
typedef struct SkyWinPlacement {
    uint64_t desk_surf;   /* desktop-side alias VA of the whole surface    */
    uint32_t w, h;        /* surface size (SKYWIN_SURF_W x SKYWIN_SURF_H)  */
    uint32_t x, y;        /* top-left position on the scanout (centered)   */
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

#ifdef __cplusplus
}
#endif
