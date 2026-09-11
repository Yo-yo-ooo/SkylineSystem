//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// TLoad: spawn the userspace console process (hw2.elf) and hand it a
// FIXED-SIZE modern (Win11-style) rounded ARGB surface to render into, WITHOUT mapping
// the real MMIO scanout into it.
//
//   share 1 (window surface)   src = hw2 (fresh zero pages, SKYWIN_W x H),
//                              dst = desktop (alias VA used to present it)
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

static void paint_console_chrome(FrameBuffer *wb) {
    const int32_t SW = (int32_t)SKYWIN_SURF_W;
    const int32_t SH = (int32_t)SKYWIN_SURF_H;
    const int32_t M  = (int32_t)SKYWIN_SHADOW;
    const float   BW = (float)SKYWIN_W, BH = (float)SKYWIN_H;
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
                else                          fill = SKYRGB_PAPER;
                out[idx] = 0xFF000000u | (fill & 0x00FFFFFFu);
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

            uint32_t fill = (by < (float)TH) ? SKYRGB_TITLE : SKYRGB_PAPER;
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
                     "Skyline Console", SKYRGB_INK);

    /* 5) close cross, centred inside the right-most caption button area */
    int32_t capX0 = M + (int32_t)SKYWIN_W - (int32_t)SKYWIN_CAPBTN_W;
    int32_t ccx   = capX0 + (int32_t)SKYWIN_CAPBTN_W / 2;
    int32_t ccy   = M + TH / 2;
    DrawLine(wb, ccx - 5, ccy - 5, ccx + 5, ccy + 5, SKYRGB_CLOSE);
    DrawLine(wb, ccx + 5, ccy - 5, ccx - 5, ccy + 5, SKYRGB_CLOSE);
}

uint64_t TLoad(FrameBuffer *Fb, SkyWinPlacement *place) {
    if (!Fb || !Fb->BaseAddress) return 0;

    uint64_t pid = sys_load((uint64_t)"/mp/hw2.elf", 0, 0);
    if ((int64_t)pid < 0) return 0;

    uint64_t self      = sys_getpid();
    uint64_t winBytes  = align_up((uint64_t)SKYWIN_SURF_W * SKYWIN_SURF_H * sizeof(uint32_t),
                                  PAGE_SIZE);

    /* share 1: hw2 owns the fresh window surface, desktop aliases it. */
    uint64_t r1 = sys_pmmapSHARE(self, 0, winBytes, SHARE_FLAGS,
                                 (uint64_t)pid, 0);
    uint64_t hw2_whole = 0, desk_whole = 0;
    read_sideband(&hw2_whole, &desk_whole);
    if ((int64_t)r1 < 0 || hw2_whole == 0 || desk_whole == 0) return 0;

    /* share 2: hw2 fixed protocol page -> desktop alias for writing. */
    uint64_t r2 = sys_pmmapSHARE(self, 0, PAGE_SIZE, SHARE_FLAGS,
                                 (uint64_t)pid, SKYWIN_PROTO_PAGE_VA);
    uint64_t proto_src = 0, proto = 0;
    read_sideband(&proto_src, &proto);
    (void)proto_src;
    if ((int64_t)r2 < 0 || proto == 0) return 0;

    /* Inner text-area origin inside the tightly-packed ARGB surface. Rows are
       strided by the SURFACE pitch (which is wider than the body, to carry the
       shadow/AA margin). */
    uint64_t content_off =
        ((uint64_t)SKYWIN_CONTENT_Y * SKYWIN_SURF_W + SKYWIN_CONTENT_X) * sizeof(uint32_t);

    memset((void*)proto, 0, PAGE_SIZE);
    volatile uint64_t *q = (volatile uint64_t*)proto;
    q[SKYWIN_PROTO_CONTENT_VA]  = hw2_whole + content_off;
    q[SKYWIN_PROTO_CONTENT_SZ]  = (uint64_t)SKYWIN_SURF_W * SKYWIN_CONTENT_H * sizeof(uint32_t);
    q[SKYWIN_PROTO_CONTENT_W]   = SKYWIN_CONTENT_W;
    q[SKYWIN_PROTO_CONTENT_H]   = SKYWIN_CONTENT_H;
    q[SKYWIN_PROTO_PITCH]       = SKYWIN_SURF_W;           /* surface pitch */
    q[SKYWIN_PROTO_WHOLE_VA]    = hw2_whole;
    q[SKYWIN_PROTO_WIN_W]       = SKYWIN_SURF_W;
    q[SKYWIN_PROTO_WIN_H]       = SKYWIN_SURF_H;

    /* The window MANAGER paints the decoration itself on its own alias of the
       shared surface, BEFORE the client is launched: zero concurrency (the
       client has not started) and the client never needs chrome code. */
    FrameBuffer wb;
    wb.BaseAddress       = (void*)desk_whole;
    wb.BufferSize        = SKYWIN_SURF_W * SKYWIN_SURF_H * sizeof(uint32_t);
    wb.Width             = SKYWIN_SURF_W;
    wb.Height            = SKYWIN_SURF_H;
    wb.PixelsPerScanLine = SKYWIN_SURF_W;      /* tightly packed ARGB surface */
    paint_console_chrome(&wb);

    if ((int64_t)sys_launch(pid) < 0) return 0;

    /* The client is a plain, portable Hello World and performs NO window/ready
       handshake at all. The chrome was fully painted above before launch, so
       the surface can never be half-filled or torn. Just yield a bounded
       number of times to let the fresh client load its font and emit its
       initial printf output before the window is mounted; any text that does
       not finish in this window is picked up by the compositor's periodic
       Compose afterwards. The client may even return from main() (and exit)
       right away: the shared physical frames survive via their refcount while
       this WM keeps the dst mapping, so the painted window stays on screen. */
    for (uint32_t s = 0; s < SKYWIN_STARTUP_YIELDS; s++) sys_yield();

    if (place) {
        place->desk_surf = desk_whole;
        place->w = SKYWIN_SURF_W;
        place->h = SKYWIN_SURF_H;
        place->x = (Fb->Width  > SKYWIN_SURF_W) ? (uint32_t)((Fb->Width  - SKYWIN_SURF_W) / 2u) : 0u;
        place->y = (Fb->Height > SKYWIN_SURF_H) ? (uint32_t)((Fb->Height - SKYWIN_SURF_H) / 2u) : 0u;
    }
    return desk_whole;
}
