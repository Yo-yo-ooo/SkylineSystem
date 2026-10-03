//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// TLoad: spawn the userspace console process (hw2.elf) and hand it a
// FIXED-SIZE modern (Win11-style) rounded ARGB surface to render into, WITHOUT mapping
// the real MMIO scanout into it.
//
//   share 1 (window surface)   src = hw2 (fresh zero pages, SKYWIN_W x H),
//                              dst = desktop (alias VA used to present it)
//     the DESKTOP rasterizes the window chrome (soft shadow, rounded body,
//     on its alias before launching the client; hw2 then ONLY runs its libc
//     flanterm printf into the inner content area. The desktop mounts the
//     SAME physical pages as one centered, non-fullscreen compositor window.
//
//   share 2 (4 KiB protocol page)  src = hw2 fixed 0x400000 (.prepad),
//                                  dst = desktop alias VA for writing.
//
// The protocol (see graphic/winstyle.h) carries TWO geometries:
//   [0..4] inner text area -> consumed verbatim by the libc console
//   [5..7] whole window    -> whole-surface geometry (chrome is owned by the
//                             desktop WM; kept for protocol completeness)
//
// The desktop-side alias + centered placement is returned through *place.

#include <stdint.h>
#include <string.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/fb.h>
#include <graphic/flanterm.h>
#include <base/font/ttf/ttf.h>
#include <graphic/winstyle.h>

#define PAGE_SIZE    4096UL
#define SHARE_FLAGS  7UL

static inline uint64_t align_up(uint64_t x, uint64_t a) {
    return (x + (a - 1)) & ~(a - 1);
}

/* Capture the rdi/rsi sideband (resolved src/dst VAs) immediately after the
   syscall returns, before the compiler can reuse those registers. */
static inline void read_sideband(uint64_t *src_va, uint64_t *dst_va) {
    __asm__ volatile("movq %%rdi, %0\n\t"
                     "movq %%rsi, %1"
                     : "=r"(*src_va), "=r"(*dst_va)
                     :: "rdi", "rsi", "memory");
}

/* ---- modern (Win11/Fluent) software rasterizer --------------------------
 * Paints ONE ARGB surface (visible body + symmetric shadow margin) off-line:
 *   - a rounded-rectangle signed-distance field gives anti-aliased 8px
 *     corners (a 1px coverage band, no jagged outline);
 *   - a second rounded box, offset down-right and fading with distance, is a
 *     soft directional drop shadow that composites over the wallpaper;
 *   - a flat cool-dark caption, a 1px luminous hairline rim, a faint caption
 *     separator and a near-black terminal paper fill the body;
 *   - the caption glyph and close cross are stamped last on the opaque bar.
 * Pixels outside body+shadow keep alpha 0, so the compositor shows wallpaper
 * through the rounded corners. This runs ONCE, before the client launches and
 * never on the per-frame compose path, so its cost is irrelevant to the
 * pointer/compositor fluidity. */
static inline float sky_fabsf(float x) { return x < 0.f ? -x : x; }
static inline float sky_fmaxf(float a, float b) { return a > b ? a : b; }
static inline float sky_clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }

/* Signed distance (px) from a box-centred point to a rounded rectangle with
   straight half-extents (rx,ry) and corner radius r: <0 inside, >0 outside. */
static inline float rounded_sdf(float px, float py, float rx, float ry, float r) {
    float qx = sky_fmaxf(sky_fabsf(px) - rx, 0.f);
    float qy = sky_fmaxf(sky_fabsf(py) - ry, 0.f);
    return __builtin_sqrtf(qx * qx + qy * qy) - r;
}

/* Stamp the three Win11 caption glyphs (minimize / maximize-or-restore /
   close), right-aligned inside a body whose top-left within the tightly
   packed surface is (bx0,by0) and whose width is bodyW. Shared by the fixed
   normal chrome (rounded shadow surface) and the full-work-area maximized
   chrome, so their click hit-areas can never drift apart. maximized != 0 draws
   the two overlapping boxes "restore" glyph on the middle button. */
void SkyPaintCaptionIcons(FrameBuffer* s, int32_t bx0, int32_t by0,
                          int32_t bodyW, int32_t titleH, int maximized) {
    const int32_t bw = (int32_t)SKYWIN_BTN_W;
    const int32_t cy = by0 + titleH / 2;

    /* minimize: one horizontal rule centred in [B-3BTN, B-2BTN) */
    int32_t cx = bx0 + (int32_t)SKYWIN_BTN_MIN_L((uint32_t)bodyW) + bw / 2;
    DrawLine(s, cx - 5, cy, cx + 5, cy, SKYRGB_CLOSE);

    /* maximize / restore: hollow square, or two overlapping squares */
    cx = bx0 + (int32_t)SKYWIN_BTN_MAX_L((uint32_t)bodyW) + bw / 2;
    if (maximized) {
        DrawRect(s, cx - 2, cy - 8, 7, 7, SKYRGB_CLOSE);    /* rear box  */
        DrawRect(s, cx - 5, cy - 5, 10, 10, SKYRGB_CLOSE);  /* front box */
    } else {
        DrawRect(s, cx - 5, cy - 5, 10, 10, SKYRGB_CLOSE);
    }

    /* close: a cross centred in the right-most button */
    cx = bx0 + (int32_t)SKYWIN_BTN_CLOSE_L((uint32_t)bodyW) + bw / 2;
    DrawLine(s, cx - 5, cy - 5, cx + 5, cy + 5, SKYRGB_CLOSE);
    DrawLine(s, cx + 5, cy - 5, cx - 5, cy + 5, SKYRGB_CLOSE);
}

/* Rasterize the modern rounded/shadowed chrome for a body of bodyW x bodyH
   placed at (M,M) inside a surface whose scanline pitch is surfW and whose
   painted height is surfH. The fixed normal surface passes the tightly-packed
   SKYWIN_SURF_* size; the WM runtime resize surface passes a fixed maximum
   pitch and only the current body height, so a resized window keeps the exact
   same Win11 look (pixels right/below the body stay alpha 0 and are skipped by
   the compositor). restoreGlyph selects the two-box "restore" caption icon. */
void SkyPaintChromeSized(FrameBuffer *wb, int32_t surfW, int32_t surfH,
                         int32_t bodyW, int32_t bodyH, int restoreGlyph,
                         const char* title, uint32_t paperRGB) {
    const int32_t SW = surfW;
    const int32_t SH = surfH;
    const int32_t M  = (int32_t)SKYWIN_SHADOW;
    const float   BW = (float)bodyW, BH = (float)bodyH;
    const float   R  = (float)SKYWIN_RADIUS;
    const int32_t TH = (int32_t)SKYWIN_TITLE_H;
    const float   HW = BW * 0.5f, HH = BH * 0.5f;
    const float   RX = HW - R, RY = HH - R;      /* straight part half-extents */

    /* soft directional shadow (key light from the upper-left) */
    const float SH_OX    = 3.f;                     /* shadow offset right    */
    const float SH_OY    = 5.f;                     /* shadow offset down     */
    const float SH_RANGE = (float)SKYWIN_SHADOW - 2.f;
    const float SH_MAX   = 0.28f;                   /* nearest shadow alpha   */

    uint32_t *out = (uint32_t*)wb->BaseAddress;
    for (int32_t y = 0; y < SH; y++) {
        float by = (float)(y - M);                 /* body-space coordinate  */
        for (int32_t x = 0; x < SW; x++) {
            float bx = (float)(x - M);
            uint32_t idx = (uint32_t)y * (uint32_t)SW + (uint32_t)x;

            /* 1) body coverage from the SDF (1px anti-aliasing band) */
            float d = rounded_sdf(bx - HW, by - HH, RX, RY, R);
            float cov;
            if (d <= -0.5f)     cov = 1.f;
            else if (d >= 0.5f) cov = 0.f;
            else                cov = 0.5f - d;

            /* Common case — strictly inside: opaque body, skip the shadow's
               second sqrt entirely. Pick the flat fill for this scanline. */
            if (cov >= 1.f) {
                uint32_t fill;
                if (d > -1.0f)                fill = SKYRGB_BORDER;   /* hairline */
                else if ((int32_t)by == TH-1) fill = SKYRGB_SEP;      /* separator*/
                else if (by < (float)TH)      fill = SKYRGB_TITLE;
                else                          fill = paperRGB;       /* client paper */                out[idx] = 0xFF000000u | (fill & 0x00FFFFFFu);
                continue;
            }

            /* 2) soft shadow: distance to the body box shifted down-right */
            float ds = rounded_sdf(bx - (HW + SH_OX), by - (HH + SH_OY), RX, RY, R);
            float sh = 0.f;
            if (ds < SH_RANGE) {
                float t = sky_clamp01(1.f - ds / SH_RANGE);
                sh = SH_MAX * t * t;             /* quadratic, soft tail     */
            }

            /* 3) composite body over the black shadow over transparent */
            float aOut = cov + sh * (1.f - cov);
            if (aOut <= 0.003f) { out[idx] = 0; continue; }

            uint32_t fill = (by < (float)TH) ? SKYRGB_TITLE : paperRGB;
            uint32_t fr = (fill >> 16) & 0xFF, fg = (fill >> 8) & 0xFF, fb = fill & 0xFF;
            /* shadow is pure black, so out_rgb = fill*cov / aOut */
            uint32_t oa = (uint32_t)(aOut * 255.f + 0.5f);
            uint32_t orr = (uint32_t)(fr * cov / aOut + 0.5f);
            uint32_t og  = (uint32_t)(fg * cov / aOut + 0.5f);
            uint32_t ob  = (uint32_t)(fb * cov / aOut + 0.5f);
            if (oa > 255) oa = 255;
            out[idx] = (oa << 24) | (orr << 16) | (og << 8) | ob;
        }
    }

    /* 4) caption glyph, left-aligned and vertically centred in the bar */
    TTF_Font *font = console_font();
    if (font)
        TTF_DrawText(wb, font, M + 14, M + (TH - 22) / 2,
                     title ? title : "", SKYRGB_INK);

    /* 5) caption glyphs: minimize / maximize / close, right-aligned */
    SkyPaintCaptionIcons(wb, M, M, bodyW, TH, restoreGlyph);
}

/* Spawn a windowed userspace app and hand it an ARGB surface (chrome painted
   by the WM before launch) plus the fixed protocol page. Generic over the elf
   path, title and body size; TLoad and the notepad launch both use this. */
uint64_t SpawnWindowedApp(FrameBuffer *Fb, const char* elf, const char* title,
                          uint32_t bodyW, uint32_t bodyH,
                          SkyWinPlacement *place, uint32_t paperRGB) {
    if (!Fb || !Fb->BaseAddress) return 0;

    uint64_t pid = sys_load((uint64_t)elf, 0, 0);
    if ((int64_t)pid < 0) return 0;

    uint64_t self      = sys_getpid();
    uint32_t surfW     = bodyW + 2u * SKYWIN_SHADOW;
    uint32_t surfH     = bodyH + 2u * SKYWIN_SHADOW;
    uint64_t winBytes  = align_up((uint64_t)surfW * surfH * sizeof(uint32_t),
                                  PAGE_SIZE);

    /* share 1: client owns the fresh window surface, desktop aliases it. */
    uint64_t r1 = sys_pmmapSHARE(self, 0, winBytes, SHARE_FLAGS,
                                 (uint64_t)pid, 0);
    uint64_t client_whole = 0, desk_whole = 0;
    read_sideband(&client_whole, &desk_whole);
    if ((int64_t)r1 < 0 || client_whole == 0 || desk_whole == 0) {
        /* P1-49: 失败回滚 —— 击杀已加载的客户端进程 */
        sys_kill(pid, 9);
        return 0;
    }

    /* share 2: client fixed protocol page -> desktop alias for writing. */
    uint64_t r2 = sys_pmmapSHARE(self, 0, PAGE_SIZE, SHARE_FLAGS,
                                 (uint64_t)pid, SKYWIN_PROTO_PAGE_VA);
    uint64_t proto_src = 0, proto = 0;
    read_sideband(&proto_src, &proto);
    (void)proto_src;
    if ((int64_t)r2 < 0 || proto == 0) {
        /* P1-49: 失败回滚 —— share 1 桌面别名归还 + 进程击杀兜底 */
        sys_munmap(desk_whole, winBytes);
        sys_kill(pid, 9);
        return 0;
    }

    /* Inner content-area origin inside the ARGB surface. */
    const uint32_t contentX = SKYWIN_SHADOW;
    const uint32_t contentY = SKYWIN_SHADOW + SKYWIN_TITLE_H;
    const uint32_t contentW = bodyW;
    const uint32_t contentH = bodyH - SKYWIN_TITLE_H - SKYWIN_RADIUS;
    uint64_t content_off =
        ((uint64_t)contentY * surfW + contentX) * sizeof(uint32_t);

    memset((void*)proto, 0, PAGE_SIZE);
    volatile uint64_t *q = (volatile uint64_t*)proto;
    q[SKYWIN_PROTO_CONTENT_VA]  = client_whole + content_off;
    q[SKYWIN_PROTO_CONTENT_SZ]  = (uint64_t)surfW * contentH * sizeof(uint32_t);
    q[SKYWIN_PROTO_CONTENT_W]   = contentW;
    q[SKYWIN_PROTO_CONTENT_H]   = contentH;
    q[SKYWIN_PROTO_PITCH]       = surfW;
    q[SKYWIN_PROTO_WHOLE_VA]    = client_whole;
    q[SKYWIN_PROTO_WIN_W]       = surfW;
    q[SKYWIN_PROTO_WIN_H]       = surfH;

    /* The WM paints the decoration on its alias BEFORE the client launches. */
    FrameBuffer wb;
    wb.BaseAddress       = (void*)desk_whole;
    wb.BufferSize        = (uint64_t)surfW * surfH * sizeof(uint32_t);
    wb.Width             = surfW;
    wb.Height            = surfH;
    wb.PixelsPerScanLine = surfW;
    SkyPaintChromeSized(&wb, (int32_t)surfW, (int32_t)surfH,
                        (int32_t)bodyW, (int32_t)bodyH, 0, title, paperRGB);

    if ((int64_t)sys_launch(pid) < 0) {
        /* P1-49: 启动失败回滚 —— 归还 share 1 桌面别名 + 击杀进程 */
        sys_munmap(desk_whole, winBytes);
        sys_kill(pid, 9);
        return 0;
    }

    /* Bounded readiness handshake (replaces the old fixed blind yield count).
       Wait for CLIENT_READY; console clients additionally wait until OUT_SEQ
       has gone quiet, i.e. the initial printf burst is fully rendered. The
       hard iteration cap guarantees a client that never signals cannot hang
       the desktop during boot. */
    {
        uint64_t last_seq = 0;
        uint32_t quiet = 0, hold = 0;
        for (uint32_t i = 0; i < SKYWIN_STARTUP_WAIT_MAX; i++) {
            uint64_t ready = q[SKYWIN_PROTO_CLIENT_READY];
            uint64_t seq   = q[SKYWIN_PROTO_OUT_SEQ];
            if (ready != SKYWIN_READY_MAGIC) {
                quiet = 0; hold = 0; last_seq = seq;
            } else if (seq != 0) {
                /* Console client: require the output counter to settle. */
                hold = 0;
                if (seq == last_seq) {
                    if (++quiet >= SKYWIN_STARTUP_QUIET) break;
                } else {
                    quiet = 0;
                    last_seq = seq;
                }
            } else {
                /* Non-console client: READY is sufficient; tiny settle. */
                if (++hold >= SKYWIN_READY_SETTLE) break;
            }
            sys_yield();
        }
    }

    if (place) {
        place->desk_surf  = desk_whole;
        place->client_pid = pid;
        place->w = surfW;
        place->h = surfH;
        place->x = (Fb->Width  > surfW) ? (uint32_t)((Fb->Width  - surfW) / 2u) : 0u;
        place->y = (Fb->Height > surfH) ? (uint32_t)((Fb->Height - surfH) / 2u) : 0u;
    }
    return desk_whole;
}

uint64_t TLoad(FrameBuffer *Fb, SkyWinPlacement *place) {
    return SpawnWindowedApp(Fb, "/mp/hw2.elf", "Skyline Console",
                            SKYWIN_W, SKYWIN_H, place, SKYRGB_PAPER);
}
