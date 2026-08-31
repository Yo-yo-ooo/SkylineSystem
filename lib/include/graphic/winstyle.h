//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// winstyle.h — shared geometry/protocol for the userspace console window.
//
// The console is a FIXED-SIZE, centered window in the style of the classic
// haribote OS ("30-nichi de Jisaku OS" / 30天自制操作系统): a raised grey
// face, a blue active title bar with a close button, and a black text area.
//
// Both desktop/TLoad (which allocates + shares the surface) and the hw2
// client (which paints the chrome and runs printf) include this header, so
// the geometry can never drift between the two sides. The window surface is
// tightly packed: its scanline pitch equals SKYWIN_W.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- outer window geometry -------------------------------------------- */
#define SKYWIN_W          620u   /* window width  (px, tightly packed)      */
#define SKYWIN_H          400u   /* window height                           */
#define SKYWIN_EDGE        5u   /* raised 3-D outer edge thickness         */
#define SKYWIN_TITLE_H    28u   /* title-bar height                        */
#define SKYWIN_PAD         6u   /* text-grid padding inside the text area  */

/* ---- inner black text (content) area, in window-surface coordinates ---- */
#define SKYWIN_CONTENT_X   SKYWIN_EDGE
#define SKYWIN_CONTENT_Y   (SKYWIN_EDGE + SKYWIN_TITLE_H)
#define SKYWIN_CONTENT_W   (SKYWIN_W - 2u * SKYWIN_EDGE)
#define SKYWIN_CONTENT_H   (SKYWIN_H - 2u * SKYWIN_EDGE - SKYWIN_TITLE_H)

/* ---- fixed protocol page (hw2 VA 0x400000), one uint64 per slot --------
 * [0..4] is EXACTLY what the libc flanterm console reads on first printf:
 *   [0] content-area VA inside hw2   [1] content bytes
 *   [2] content width               [3] content height   [4] pitch
 * [5..7] describe the whole window surface, used by hw2 to paint chrome:
 *   [5] whole-surface VA inside hw2 [6] SKYWIN_W         [7] SKYWIN_H
 * [8] first-frame-ready handshake: hw2 publishes SKYWIN_READY_MAGIC (release)
 *     only after the WHOLE surface (chrome + initial console text) is painted,
 *     so the desktop never composites a half-filled / torn surface. */
#define SKYWIN_PROTO_CONTENT_VA   0u
#define SKYWIN_PROTO_CONTENT_SZ   1u
#define SKYWIN_PROTO_CONTENT_W    2u
#define SKYWIN_PROTO_CONTENT_H    3u
#define SKYWIN_PROTO_PITCH        4u
#define SKYWIN_PROTO_WHOLE_VA     5u
#define SKYWIN_PROTO_WIN_W        6u
#define SKYWIN_PROTO_WIN_H        7u
#define SKYWIN_PROTO_READY        8u   /* hw2 -> desktop first-frame ready */
#define SKYWIN_PROTO_SLOTS        16u
#define SKYWIN_PROTO_PAGE_VA  0x400000UL  /* hw2 fixed .prepad protocol page */

/* "SKYRDY1" little-endian tag written by hw2 when its first frame is done. */
#define SKYWIN_READY_MAGIC     0x31594452594B5355ULL
/* Bounded spin/yield wait for that handshake before the window is mounted. */
#define SKYWIN_READY_WAIT_LIMIT  8000000ULL

/* Placement returned by TLoad to the desktop compositor. */
typedef struct SkyWinPlacement {
    uint64_t desk_surf;   /* desktop-side alias VA of the whole surface    */
    uint32_t w, h;        /* window size  (SKYWIN_W x SKYWIN_H)            */
    uint32_t x, y;        /* top-left position on the scanout (centered)   */
} SkyWinPlacement;

/* ---- haribote palette (0xAARRGGBB, matches RGB() in graphic/fb.h) ----- */
#define SKYRGB_FACE     0xFFC6C6C6u  /* window face, light grey             */
#define SKYRGB_LIGHT    0xFFFFFFFFu  /* raised highlight (top/left edge)     */
#define SKYRGB_SHADOW   0xFF888888u  /* recessed shadow  (bottom/right edge) */
#define SKYRGB_DARK     0xFF555555u  /* deeper shadow / close glyph          */
#define SKYRGB_TITLE    0xFF0A34DEu  /* active title-bar blue                */
#define SKYRGB_PAPER    0xFF000000u  /* text-area paper, black               */
#define SKYRGB_INK      0xFFFFFFFFu  /* title glyph ink, white               */

#ifdef __cplusplus
}
#endif
