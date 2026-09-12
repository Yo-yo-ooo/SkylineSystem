//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <graphic/fb.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/basicdraw.hpp>
#include <graphic/winstyle.h>
#include <graphic/flanterm.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <base/font/ttf/ttf.h>
#include <mouse/ps2.h>
#include <synthesizer/window.h>
static char intTo_stringOutput[128];

uint64_t TLoad(FrameBuffer *Fb, SkyWinPlacement *place);

/* Caption glyph painter + sized chrome rasterizer, both defined in loader.cpp */
void SkyPaintCaptionIcons(FrameBuffer* s, int32_t bx0, int32_t by0,
                          int32_t bodyW, int32_t titleH, int maximized);
void SkyPaintChromeSized(FrameBuffer* s, int32_t surfW, int32_t surfH,
                         int32_t bodyW, int32_t bodyH, int restoreGlyph);

// 处理无符号 64 位整数
const char *to_string(uint64_t value)
{
    uint8_t i = 0;
    if (value == 0) {
        intTo_stringOutput[i++] = '0';
        intTo_stringOutput[i] = '\0';
        return intTo_stringOutput;
    }
    while (value > 0) {
        intTo_stringOutput[i++] = (value % 10) + '0';
        value /= 10;
    }
    intTo_stringOutput[i] = '\0';
    uint8_t left = 0;
    uint8_t right = i - 1;
    while (left < right) {
        char temp = intTo_stringOutput[left];
        intTo_stringOutput[left] = intTo_stringOutput[right];
        intTo_stringOutput[right] = temp;
        left++;
        right--;
    }
    return intTo_stringOutput;
}
const char *to_string(int64_t value)
{
    if (value < 0) {
        uint64_t u_val = -value;
        const char* num_str = to_string(u_val);
        uint8_t len = 0;
        while (num_str[len] != '\0') len++;
        for (int8_t j = len; j >= 0; j--) {
            intTo_stringOutput[j + 1] = intTo_stringOutput[j];
        }
        intTo_stringOutput[0] = '-';
        return intTo_stringOutput;
    }
    return to_string((uint64_t)value);
}

extern void DrawMousePointer(int32_t mousex,int32_t mousey, FrameBuffer* framebuffer);

/* ---- monotonic TSC frame-pacing helpers -----------------------------------
 * The kernel's uptime_ms is only refreshed by the idle thread, which does
 * not run while this main loop is RUNNABLE, so it cannot pace the cursor.
 * Derive cycles/ms directly from CPUID 0x15 (crystal*num/den) with 0x16
 * (base MHz) and a conservative 3 GHz fallback. */
static inline uint64_t rdtsc64() {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t probe_tsc_per_ms() {
    uint32_t a, b, c, d;
    a = 0x15; c = 0;                                   /* TSC / crystal ratio */
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                    : "0"(a), "2"(c) : "memory");
    if (a && b && c) return (uint64_t)c * b / a / 1000u;     /* Hz -> /ms    */
    a = 0x16; c = 0;                                   /* processor base MHz */
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                    : "0"(a), "2"(c) : "memory");
    if (a) return (uint64_t)a * 1000u;               /* MHz -> cycles/ms      */
    return 3000000u;                                  /* assume 3 GHz         */
}

const char* to_string(char value)
{
    intTo_stringOutput[0] = value;
    intTo_stringOutput[1] = '\0';
    return intTo_stringOutput;
}

/* ========================================================================== */
/*  Window-manager helpers: acrylic taskbar, clock/power tray, geometry,      */
/*  runtime resize surface and 1:1 client-content mirror.                     */
/* ========================================================================== */

/* Integer source-over used for the taskbar's acrylic darkening. */
static inline uint32_t wm_acrylic_pixel(uint32_t wallpaper) {
    const uint32_t k  = SKY_ACRYLIC_KEEP;
    const uint32_t ik = 255u - k;
    uint32_t wr = (wallpaper >> 16) & 0xFF, wg = (wallpaper >> 8) & 0xFF, wb = wallpaper & 0xFF;
    uint32_t br = (SKY_ACRYLIC_BASE >> 16) & 0xFF,
             bg = (SKY_ACRYLIC_BASE >> 8)  & 0xFF, bb = SKY_ACRYLIC_BASE & 0xFF;
    uint32_t r = (wr * k + br * ik) / 255u;
    uint32_t g = (wg * k + bg * ik) / 255u;
    uint32_t b = (wb * k + bb * ik) / 255u;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

/* Compact battery glyph: a rounded outline, a positive nub and a near-full
   green fill. This VM/desktop has no battery bus or ACPI control-method
   battery, so the icon denotes "on external power / charged" rather than a
   measured percentage; wire a real gauge here once a battery driver exists. */
static void wm_draw_battery(FrameBuffer* fb, int32_t x, int32_t y, uint32_t ink) {
    DrawRect(fb, x, y, 19, 12, ink);                 /* hollow body outline   */
    DrawFillRect(fb, x + 19, y + 3, 2, 6, ink);      /* positive nub          */
    DrawFillRect(fb, x + 2, y + 2, 14, 8, 0xFF76D9A4u); /* ~90% green charge  */
}

/* Paint the bottom taskbar straight into the wallpaper bitmap (compositor
   layer 0). `cleanBar` is the pristine wallpaper strip; it is restored first
   so repeatedly repainting (clock tick / state change) never compounds the
   acrylic darkening. appState: 0 idle (shown), 1 active (minimized),
   2 closed (no app entry). */
static void wm_draw_taskbar(uint32_t* wall, const uint32_t* cleanBar,
                            uint32_t W, uint32_t H, int appState,
                            int hh, int mm, bool haveClock) {
    FrameBuffer lb;
    lb.BaseAddress       = wall;
    lb.BufferSize        = (uint64_t)W * H * sizeof(uint32_t);
    lb.Width = lb.PixelsPerScanLine = W;
    lb.Height            = H;

    const uint32_t barH = SKYWIN_TASKBAR_H;
    const uint32_t y0   = H - barH;

    /* 1) restore the pristine wallpaper strip, then darken it in place so the
          wallpaper glows through (Win11 acrylic / macOS translucency). */
    memcpy(wall + (uint64_t)y0 * W, cleanBar, (size_t)W * barH * sizeof(uint32_t));
    uint32_t* stripe = wall + (uint64_t)y0 * W;
    for (uint32_t i = 0; i < W * barH; i++) stripe[i] = wm_acrylic_pixel(stripe[i]);
    DrawFillRect(&lb, 0, y0, W, 1, SKYRGB_TB_HILITE);   /* luminous top edge  */

    /* 2) left: rounded app pill (hidden once the window is closed) */
    if (appState != 2) {
        const int32_t bx = 10, by = (int32_t)y0 + 7, bw = 208, bh = (int32_t)barH - 14;
        BasicDraw pill(&lb);
        const uint32_t face = appState == 1 ? SKYRGB_TBTN_ON : SKYRGB_TBTN_IDLE;
        pill.DrawRoundedRect(bx, by, bw, bh, 7, face, true);
        pill.DrawRoundedRect(bx, by, bw, bh, 7, appState == 1 ? SKYRGB_ACCENT : SKYRGB_BORDER, false);
        if (appState == 1) DrawFillRect(&lb, bx + 2, by + 5, 3, bh - 10, SKYRGB_ACCENT);
        TTF_Font* f = console_font();
        if (f)
            TTF_DrawText(&lb, f, bx + 16, by + (bh - 22) / 2,
                         "Skyline Console", appState == 1 ? 0xFFFFFFFFu : SKYRGB_INK);
    }

    /* 3) right tray: battery glyph + local HH:MM, right aligned */
    char clk[6];
    clk[0] = (char)('0' + hh / 10); clk[1] = (char)('0' + hh % 10);
    clk[2] = ':';
    clk[3] = (char)('0' + mm / 10); clk[4] = (char)('0' + mm % 10);
    clk[5] = '\0';

    TTF_Font* tf = console_font();
    int32_t tw = 62, th = 22;
    if (tf && haveClock) TTF_GetTextSize(tf, clk, &tw, &th);
    int32_t clkX = (int32_t)W - (int32_t)SKY_TRAY_MARGIN - (haveClock ? tw : 0);
    int32_t clkY = (int32_t)y0 + ((int32_t)barH - 22) / 2;
    int32_t batX = clkX - 10 - 21;
    int32_t batY = (int32_t)y0 + ((int32_t)barH - 12) / 2;
    wm_draw_battery(&lb, batX, batY, SKYRGB_TRAY_INK);
    if (tf && haveClock)
        TTF_DrawText(&lb, tf, clkX, clkY, clk, SKYRGB_TRAY_INK);
}

/* Read the wall clock via SYSCALL_TIME (RTC civil seconds, UTC). mktime is
   linear in H/M/S, so total mod 86400 recovers H*3600+M*60+S regardless of
   the calendar fields; shift to local time by SKY_LOCAL_TZ_MIN. */
static bool wm_read_clock(int* hh, int* mm) {
    int64_t s = (int64_t)syscall(SYSCALL_TIME, 0, 0, 0, 0, 0, 0);
    if (s < 0) return false;
    uint64_t local = (uint64_t)(s + (int64_t)SKY_LOCAL_TZ_MIN * 60) % 86400u;
    *hh = (int)(local / 3600u);
    *mm = (int)((local / 60u) % 60u);
    return true;
}

/* Switch the console window to its rounded NORMAL presentation. When the body
   is still the fixed SKYWIN_W x H it uses the zero-copy shared client surface;
   once it has been resized it uses the WM-owned fixed-pitch rzSurf. Called only
   between synchronous Compose() frames (workers parked on the barrier). */
static void wm_apply_normal(Window* w, const SkyWinPlacement* pl,
                           uint32_t nx, uint32_t ny, uint32_t nw, uint32_t nh,
                           const uint32_t* rzSurf, int32_t rzPitch) {
    w->PosX = nx;  w->PosY = ny;
    if (rzSurf && (nw != SKYWIN_W || nh != SKYWIN_H)) {
        w->SizeX = (uint32_t)rzPitch;
        w->SizeY = nh + 2u * SKYWIN_SHADOW;
        w->FbAddr = (uint64_t)rzSurf;
        w->HasAlpha = 1;
    } else {
        w->SizeX = SKYWIN_SURF_W;
        w->SizeY = SKYWIN_SURF_H;
        w->FbAddr = pl->desk_surf;
        w->HasAlpha = 1;
    }
}

/* Switch the console window to the opaque, full-work-area MAXIMIZED surface. */
static void wm_apply_max(Window* w, const uint32_t* maxSurf,
                        uint32_t mw, uint32_t mh) {
    w->PosX = 0;  w->PosY = 0;
    w->SizeX = mw;  w->SizeY = mh;
    w->FbAddr = (uint64_t)maxSurf;
    w->HasAlpha = 0;
}

/* Mirror the client's fixed terminal bitmap 1:1 into a WM-owned presentation
   surface. No scaling, so glyphs stay crisp: the visible content is clipped to
   CONTENT_W x CONTENT_H (a smaller window crops it, a larger one leaves the
   rest as the already-painted black paper). */
static void wm_mirror_content(uint32_t* dst, int32_t dstPitch,
                              int32_t dx, int32_t dy,
                              int32_t bodyW, int32_t bodyH,
                              const uint32_t* normSurf) {
    int32_t cw = bodyW;
    if (cw > (int32_t)SKYWIN_CONTENT_W) cw = (int32_t)SKYWIN_CONTENT_W;
    int32_t ch = bodyH - (int32_t)SKYWIN_TITLE_H - (int32_t)SKYWIN_RADIUS;
    if (ch > (int32_t)SKYWIN_CONTENT_H) ch = (int32_t)SKYWIN_CONTENT_H;
    if (cw <= 0 || ch <= 0) return;
    for (int32_t r = 0; r < ch; r++) {
        const uint32_t* sp = normSurf
            + (uint64_t)((int32_t)SKYWIN_CONTENT_Y + r) * SKYWIN_SURF_W
            + SKYWIN_CONTENT_X;
        uint32_t* dp = dst + (uint64_t)(dy + r) * (uint32_t)dstPitch + dx;
        memcpy(dp, sp, (size_t)cw * sizeof(uint32_t));
    }
}

/* (Re)paint the fixed-pitch resize surface for a body of nw x nh: clear the
   valid rows so a shrink leaves no stale edge, rasterize the rounded chrome at
   the new size, then mirror the latest client text. */
static void wm_rebuild_resize(uint32_t* rzSurf, int32_t rzPitch,
                              int32_t nw, int32_t nh, const uint32_t* normSurf) {
    const int32_t M  = (int32_t)SKYWIN_SHADOW;
    const int32_t rows = nh + 2 * M;
    memset(rzSurf, 0, (size_t)rzPitch * (uint32_t)rows * sizeof(uint32_t));
    FrameBuffer rb;
    rb.BaseAddress       = rzSurf;
    rb.BufferSize        = (uint64_t)rzPitch * rows * sizeof(uint32_t);
    rb.Width = rb.PixelsPerScanLine = rzPitch;
    rb.Height            = rows;
    SkyPaintChromeSized(&rb, rzPitch, rows, nw, nh, 0);
    wm_mirror_content(rzSurf, rzPitch, M, M + (int32_t)SKYWIN_TITLE_H,
                      nw, nh, normSurf);
}

/* Cheap LIVE preview painted on every drag step: flat rectangles, square
   corners, caption glyphs and the 1:1 client mirror, but NO per-pixel SDF and
   NO soft shadow. The full rounded/shadowed chrome is rasterized exactly once
   when the drag ends (wm_rebuild_resize). Keeping the drag frame O(area) with
   only linear fills prevents the heavy SDF raster (which on a slow/emulated
   core starves the PS/2 IRQ and drops movement packets, so the drag would
   lag its own cursor) and matches how mainstream WMs show a lightweight
   outline while resizing. */
static void wm_paint_chrome_live(uint32_t* rzSurf, int32_t rzPitch,
                                 int32_t nw, int32_t nh,
                                 const uint32_t* normSurf) {
    const int32_t M  = (int32_t)SKYWIN_SHADOW;
    const int32_t TH = (int32_t)SKYWIN_TITLE_H;
    const int32_t rows = nh + 2 * M;
    memset(rzSurf, 0, (size_t)rzPitch * (uint32_t)rows * sizeof(uint32_t));
    FrameBuffer lb;
    lb.BaseAddress       = rzSurf;
    lb.BufferSize        = (uint64_t)rzPitch * rows * sizeof(uint32_t);
    lb.Width = lb.PixelsPerScanLine = rzPitch;
    lb.Height            = rows;
    DrawFillRect(&lb, M, M, nw, TH, SKYRGB_TITLE);                 /* title bar */
    DrawFillRect(&lb, M, M + TH, nw, nh - TH, SKYRGB_PAPER);       /* paper     */
    DrawFillRect(&lb, M, M + TH - 1, nw, 1, SKYRGB_SEP);           /* separator */
    DrawRect(&lb, M, M, nw, nh, SKYRGB_BORDER);                    /* hair frame*/
    SkyPaintCaptionIcons(&lb, M, M, nw, TH, 0);                    /* glyphs    */
    wm_mirror_content(rzSurf, rzPitch, M, M + TH, nw, nh, normSurf);
}


int main(){
    FrameBuffer fb;
    uint64_t FbAddr = MapFB();
    fb = GetFBInfo();
    fb.BaseAddress = (void*)FbAddr;

    uint32_t scrW = (uint32_t)fb.Width;
    uint32_t scrH = (uint32_t)fb.Height;
    size_t wallBytes = (size_t)scrW * scrH * sizeof(uint32_t);
    uint32_t* wallBuf = (uint32_t*)malloc(wallBytes);
    if (wallBuf == nullptr) return 1;

    const uint32_t barH = SKYWIN_TASKBAR_H;

    BasicDraw bd((FrameBuffer*)&fb);
    bd.RenderWallpaper(wallBuf);

    /* Pristine copy of just the taskbar strip, so the acrylic layer can be
       repainted every minute without darkening on top of itself. */
    uint32_t* cleanBar = (uint32_t*)malloc((size_t)scrW * barH * sizeof(uint32_t));
    if (cleanBar)
        memcpy(cleanBar, wallBuf + (uint64_t)(scrH - barH) * scrW,
               (size_t)scrW * barH * sizeof(uint32_t));

    Compositor& comp = Compositor::Get();
    if (!comp.Init((FrameBuffer*)&fb)) return 1;

    int bootHH = 0, bootMM = 0;
    bool haveClock = wm_read_clock(&bootHH, &bootMM);
    if (cleanBar)
        wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, 0, bootHH, bootMM, haveClock);

    static Window wallpaperWin;
    wallpaperWin.PosX = wallpaperWin.PosY = 0;
    wallpaperWin.SizeX = scrW;
    wallpaperWin.SizeY = scrH;
    wallpaperWin.FrameStartX = wallpaperWin.FrameStartY = 0;
    wallpaperWin.FrameEndX = scrW;
    wallpaperWin.FrameEndY = scrH;
    wallpaperWin.FbAddr = (uint64_t)wallBuf;

    static SkyWinPlacement place;
    uint64_t consoleSurf = TLoad(&fb, &place);

    CompLayer* layer0 = comp.CreateLayer(0);
    comp.RegisterWindow(&wallpaperWin, layer0);

    static Window consoleWin;
    if (consoleSurf && place.desk_surf) {
        consoleWin.PosX = place.x;
        consoleWin.PosY = place.y;
        consoleWin.SizeX = place.w;
        consoleWin.SizeY = place.h;
        consoleWin.FrameStartX = SKYWIN_CONTENT_X;
        consoleWin.FrameStartY = SKYWIN_CONTENT_Y;
        consoleWin.FrameEndX   = SKYWIN_CONTENT_X + SKYWIN_CONTENT_W;
        consoleWin.FrameEndY   = SKYWIN_CONTENT_Y + SKYWIN_CONTENT_H;
        consoleWin.FbAddr = place.desk_surf;
        consoleWin.HasAlpha = 1;   /* rounded corners + soft drop shadow      */
        CompLayer* layer1 = comp.CreateLayer(1);
        comp.RegisterWindow(&consoleWin, layer1);
    }

    /* Desktop-owned (NOT shared with the client) full-work-area surface used
       only while maximized: opaque, no rounded shadow margin. The live text is
       mirrored in from the normal shared surface each frame. */
    const uint32_t maxW = scrW;
    const uint32_t maxH = scrH - barH;
    uint32_t* maxSurf = (uint32_t*)malloc((size_t)maxW * maxH * sizeof(uint32_t));
    if (maxSurf) {
        FrameBuffer mb;
        mb.BaseAddress       = maxSurf;
        mb.BufferSize        = (uint64_t)maxW * maxH * sizeof(uint32_t);
        mb.Width = mb.PixelsPerScanLine = maxW;
        mb.Height            = maxH;
        DrawFillRect(&mb, 0, 0, maxW, SKYWIN_TITLE_H, SKYRGB_TITLE);
        DrawFillRect(&mb, 0, SKYWIN_TITLE_H, maxW, maxH - SKYWIN_TITLE_H, SKYRGB_PAPER);
        DrawFillRect(&mb, 0, SKYWIN_TITLE_H - 1, maxW, 1, SKYRGB_SEP);
        TTF_Font* mf = console_font();
        if (mf)
            TTF_DrawText(&mb, mf, 14, (SKYWIN_TITLE_H - 22) / 2,
                         "Skyline Console", SKYRGB_INK);
        SkyPaintCaptionIcons(&mb, 0, 0, (int32_t)maxW,
                             (int32_t)SKYWIN_TITLE_H, 1);
    }

    /* WM-owned fixed-pitch surface used after an interactive edge resize. Its
       pitch is the maximum work-area width (plus shadow margin) so a drag that
       changes width never reallocates; unused right-hand columns stay alpha 0
       and are skipped by the alpha blender. */
    const int32_t M  = (int32_t)SKYWIN_SHADOW;
    const int32_t rzPitch = (int32_t)maxW + 2 * M;
    const int32_t rzCapRows = (int32_t)maxH + 2 * M;
    uint32_t* rzSurf = nullptr;

    comp.StartWorkers();

    for (int warm = 0; warm < 8; warm++) {
        comp.Compose();
        sys_yield();
    }

    MouseInit();

    /* Two independent layers:
       - SCENE (wallpaper + windows): composited off-screen and presented at
         a modest rate -- it changes slowly.
       - POINTER: its own layer written straight to the scanout. A move only
         restores the saved 16x16 backdrop and re-stamps the arrow
         (CursorMoveTo): O(16^2), workers stay asleep, no full-screen copy. */
    const uint64_t tsc_per_ms = probe_tsc_per_ms();
    const uint64_t move_gap   = 16u  * tsc_per_ms;   /* pointer overlay ~60Hz */
    const uint64_t scene_gap  = 33u  * tsc_per_ms;   /* recompose scene ~30Hz */
    const uint64_t idle_gap   = 50u  * tsc_per_ms;   /* scene refresh, still  */
    const uint64_t chrome_gap = 16u  * tsc_per_ms;   /* live-resize repaint  */
    const uint64_t clock_gap  = 500u * tsc_per_ms;   /* poll RTC twice / min */

    int32_t prev_x = -100;
    int32_t prev_y = -100;

    int32_t fb_width = (int32_t)fb.Width;
    int32_t fb_height = (int32_t)fb.Height;

    ps2_mouse_state_t *p = (ps2_mouse_state_t*)mouse_addr;

    uint32_t seq1, seq2;
    int32_t mx, my;
    uint64_t last_move  = rdtsc64();
    uint64_t last_scene = rdtsc64();
    uint64_t last_chrome = 0;
    uint64_t last_clock = 0;

    /* ---- window-manager interaction state ---- */
    enum { WM_NORMAL = 0, WM_MAX = 1, WM_MIN = 2, WM_CLOSED = 3 } wmMode = WM_NORMAL;
    uint32_t normX = place.x, normY = place.y;   /* NORMAL surface top-left   */
    uint32_t normW = SKYWIN_W, normH = SKYWIN_H; /* NORMAL body size          */
    bool     prevLeft = false, dragging = false, resizing = false;
    int32_t  grabDX = 0, grabDY = 0;
    uint8_t  rzDir = 0;   /* edge resize directions: bit0 L,1 R,2 T,3 B       */
    int32_t  rsX = 0, rsY = 0, rsW = 0, rsH = 0, rsMX = 0, rsMY = 0;
    int      pressHit = 0;   /* 0 none,1 caption,2 min,3 max,4 close,5 tb,6 rz */
    uint8_t  ml = 0;         /* left-button snapshot from the seqlock block    */
    int      clkHH = bootHH, clkMM = bootMM;

    comp.SetCursor(0, 0, true);

    for(;;){
        /* take the newest available mouse snapshot (seqlock retry) */
        while (true) {
            seq1 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
            if (seq1 & 1) continue;
            mx = p->x;
            my = p->y;
            ml = p->left;
            seq2 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
            if (seq1 == seq2) break;
        }

        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= fb_width - 16) mx = fb_width - 16;
        if (my >= fb_height - 16) my = fb_height - 16;

        uint64_t now = rdtsc64();
        bool moved = (mx != prev_x || my != prev_y);

        /* ==================== Window-manager interaction ==================== */
        const bool leftDown = (ml != 0);
        const int32_t th = (int32_t)SKYWIN_TITLE_H;

        /* Current BODY rectangle on screen (the visible rect). */
        int32_t bx, by, bw, bh;
        if (wmMode == WM_MAX) { bx = 0; by = 0; bw = (int32_t)maxW; bh = (int32_t)maxH; }
        else { bx = (int32_t)normX + M; by = (int32_t)normY + M;
               bw = (int32_t)normW;   bh = (int32_t)normH; }

        /* taskbar app-pill hit rectangle (matches wm_draw_taskbar layout) */
        const int32_t tbX0 = 10, tbW = 208;
        const int32_t tbY0 = fb_height - (int32_t)barH + 7;
        const int32_t tbH  = (int32_t)barH - 14;
        auto inBox = [&](int32_t x0, int32_t y0, int32_t ww, int32_t hh) {
            return mx >= x0 && mx < x0 + ww && my >= y0 && my < y0 + hh;
        };
        bool wmDirty = false;

        if (leftDown && !prevLeft) {                 /* press edge: classify  */
            pressHit = 0; rzDir = 0;
            if (wmMode == WM_NORMAL || wmMode == WM_MAX) {
                int32_t minL   = bx + bw - 3 * (int32_t)SKYWIN_BTN_W;
                int32_t maxL   = bx + bw - 2 * (int32_t)SKYWIN_BTN_W;
                int32_t closeL = bx + bw - 1 * (int32_t)SKYWIN_BTN_W;
                if      (inBox(closeL, by, (int32_t)SKYWIN_BTN_W, th)) pressHit = 4;
                else if (inBox(maxL,   by, (int32_t)SKYWIN_BTN_W, th)) pressHit = 3;
                else if (inBox(minL,   by, (int32_t)SKYWIN_BTN_W, th)) pressHit = 2;
                else if (wmMode == WM_NORMAL) {
                    /* edge/corner resize band takes precedence over caption  */
                    int32_t dL = mx - bx, dR = bx + bw - 1 - mx;
                    int32_t dT = my - by, dB = by + bh - 1 - my;
                    const int32_t RB = (int32_t)SKYWIN_RESIZE_BORDER;
                    /* The grab band straddles the frame: it reaches a few
                       pixels *outside* the body (over the shadow), exactly as
                       desktop WM do, so an edge is catchable from both sides. */
                    const int32_t OUT = 4;
                    /* Orthogonal spans also straddle the frame so a *corner*
                       is catchable even when the pointer sits a few px outside
                       both edges at once (e.g. the top-left corner). */
                    bool spanX = mx >= bx - OUT && mx < bx + bw + OUT;
                    bool spanY = my >= by - OUT && my < by + bh + OUT;
                    bool onL = spanY && dL >= -OUT && dL < RB;
                    bool onR = spanY && dR >= -OUT && dR < RB;
                    bool onT = spanX && dT >= -OUT && dT < RB;
                    bool onB = spanX && dB >= -OUT && dB < RB;
                    if (onL || onR || onT || onB) {
                        rzDir = (uint8_t)((onL?1:0)|(onR?2:0)|(onT?4:0)|(onB?8:0));
                        pressHit = 6;
                        resizing = true;
                        rsX = (int32_t)normX; rsY = (int32_t)normY;
                        rsW = (int32_t)normW; rsH = (int32_t)normH;
                        rsMX = mx; rsMY = my;
                        last_chrome = 0;             /* repaint immediately   */
                    } else if (inBox(bx, by, bw, th)) {
                        pressHit = 1;                /* caption empty area    */
                        dragging = true;
                        grabDX = mx - (int32_t)normX;
                        grabDY = my - (int32_t)normY;
                    }
                }
            }
            if (pressHit == 0 && wmMode != WM_CLOSED &&
                inBox(tbX0, tbY0, tbW, tbH)) pressHit = 5;
        }

        if (leftDown && dragging && wmMode == WM_NORMAL) {       /* caption move */
            int32_t nx = mx - grabDX, ny = my - grabDY;
            const int32_t xLo = -((int32_t)normW + 2 * M - 120);
            const int32_t xHi = fb_width  - 120;
            const int32_t yLo = -M;
            const int32_t yHi = fb_height - (int32_t)barH - th - M;
            if (nx < xLo) nx = xLo; if (nx > xHi) nx = xHi;
            if (ny < yLo) ny = yLo; if (ny > yHi) ny = yHi;
            normX = (uint32_t)nx; normY = (uint32_t)ny;
            comp.MoveWindow(&consoleWin, normX, normY);
        }

        if (leftDown && resizing && wmMode == WM_NORMAL) {      /* edge resize */
            int32_t dx = mx - rsMX, dy = my - rsMY;
            int32_t nw = rsW, nh = rsH, nx = rsX, ny = rsY;
            if (rzDir & 2) nw = rsW + dx;                       /* right      */
            if (rzDir & 8) nh = rsH + dy;                       /* bottom     */
            if (rzDir & 1) { nw = rsW - dx; nx = rsX + dx; }    /* left       */
            if (rzDir & 4) { nh = rsH - dy; ny = rsY + dy; }    /* top        */
            const int32_t minW = (int32_t)SKYWIN_MIN_W, minH = (int32_t)SKYWIN_MIN_H;
            if (nw < minW) { if (rzDir & 1) nx -= (minW - nw); nw = minW; }
            if (nh < minH) { if (rzDir & 4) ny -= (minH - nh); nh = minH; }
            if (nw > (int32_t)maxW) nw = (int32_t)maxW;
            if (nh > (int32_t)maxH) nh = (int32_t)maxH;
            if (nx < -M) { nw += (nx + M); nx = -M; if (nw < minW) nw = minW; }
            if (ny < -M) { nh += (ny + M); ny = -M; if (nh < minH) nh = minH; }

            normX = (uint32_t)nx; normY = (uint32_t)ny;
            normW = (uint32_t)nw; normH = (uint32_t)nh;
            if (!rzSurf)
                rzSurf = (uint32_t*)malloc((size_t)rzPitch * rzCapRows * sizeof(uint32_t));
            /* Repaint at a bounded rate; the pointer overlay still runs at
               its own 60 Hz, so the hand never stutters between resizes. */
            if (rzSurf && (now - last_chrome >= chrome_gap || last_chrome == 0)) {
                wm_paint_chrome_live(rzSurf, rzPitch, nw, nh,
                                     (const uint32_t*)place.desk_surf);
                wm_apply_normal(&consoleWin, &place, normX, normY, normW, normH,
                                rzSurf, rzPitch);
                last_chrome = now;
                comp.Compose();
                last_scene = now;
            }
        }

        if (!leftDown && prevLeft) {                 /* release edge: action  */
            bool fire = false;
            if (pressHit >= 2 && pressHit <= 4 &&
                (wmMode == WM_NORMAL || wmMode == WM_MAX)) {
                int32_t lx = bx + bw - (5 - pressHit) * (int32_t)SKYWIN_BTN_W;
                fire = inBox(lx, by, (int32_t)SKYWIN_BTN_W, th);
            } else if (pressHit == 5) {
                fire = inBox(tbX0, tbY0, tbW, tbH);
            }

            /* Finish a live resize at the exact release geometry. */
            if (resizing) {
                if (!rzSurf)
                    rzSurf = (uint32_t*)malloc((size_t)rzPitch * rzCapRows * sizeof(uint32_t));
                if (rzSurf) {
                    wm_rebuild_resize(rzSurf, rzPitch, (int32_t)normW, (int32_t)normH,
                                      (const uint32_t*)place.desk_surf);
                    wm_apply_normal(&consoleWin, &place, normX, normY, normW, normH,
                                    rzSurf, rzPitch);
                    wmDirty = true;
                }
            }
            dragging = false;
            resizing = false;

            if (fire) {
                int tbState = (wmMode == WM_MIN) ? 1 : 0;
                if (pressHit == 2) {                        /* minimize         */
                    wmMode = WM_MIN;
                    comp.SetVisible(&consoleWin, false);
                    wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, 1, clkHH, clkMM, haveClock);
                    wmDirty = true;
                } else if (pressHit == 4) {                 /* close: unregister */
                    wmMode = WM_CLOSED;
                    comp.UnregisterWindow(&consoleWin);
                    wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, 2, clkHH, clkMM, haveClock);
                    wmDirty = true;
                } else if (pressHit == 3 && (maxSurf)) {    /* maximize toggle  */
                    if (wmMode == WM_NORMAL) {
                        wmMode = WM_MAX;
                        wm_apply_max(&consoleWin, maxSurf, maxW, maxH);
                    } else if (wmMode == WM_MAX) {
                        wmMode = WM_NORMAL;
                        wm_apply_normal(&consoleWin, &place, normX, normY,
                                        normW, normH, rzSurf, rzPitch);
                    }
                    wmDirty = true;
                } else if (pressHit == 5) {                 /* taskbar toggle   */
                    if (wmMode == WM_MIN) {
                        wmMode = WM_NORMAL;
                        wm_apply_normal(&consoleWin, &place, normX, normY,
                                        normW, normH, rzSurf, rzPitch);
                        comp.SetVisible(&consoleWin, true);
                        tbState = 0;
                    } else if (wmMode == WM_NORMAL) {
                        wmMode = WM_MIN;
                        comp.SetVisible(&consoleWin, false);
                        tbState = 1;
                    }
                    wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, tbState,
                                    clkHH, clkMM, haveClock);
                    wmDirty = true;
                }
            }
            pressHit = 0;
        }
        prevLeft = leftDown;

        /* Keep the mirrored presentation surfaces in step with the client. */
        if (wmMode == WM_MAX && maxSurf)
            wm_mirror_content(maxSurf, (int32_t)maxW, 0, (int32_t)SKYWIN_TITLE_H,
                              (int32_t)maxW, (int32_t)maxH,
                              (const uint32_t*)place.desk_surf);
        else if (wmMode == WM_NORMAL && rzSurf &&
                 (normW != SKYWIN_W || normH != SKYWIN_H))
            wm_mirror_content(rzSurf, rzPitch, M, M + th,
                              (int32_t)normW, (int32_t)normH,
                              (const uint32_t*)place.desk_surf);

        /* Poll the wall clock twice a minute; repaint the tray on change. */
        if (cleanBar && now - last_clock >= clock_gap) {
            last_clock = now;
            int nhh = clkHH, nmm = clkMM;
            if (wm_read_clock(&nhh, &nmm) && (nhh != clkHH || nmm != clkMM)) {
                clkHH = nhh; clkMM = nmm;
                int tstate = (wmMode == WM_MIN) ? 1 : (wmMode == WM_CLOSED ? 2 : 0);
                wm_draw_taskbar(wallBuf, cleanBar, scrW, scrH, tstate,
                                clkHH, clkMM, true);
                wmDirty = true;
            }
        }

        if (wmDirty) { comp.Compose(); last_scene = rdtsc64(); }

        if (moved) {
            /* Pointer overlay fast path: spin-pace at ~60Hz, then erase and
               re-stamp only the 16x16 square directly on the scanout. */
            if (now - last_move < move_gap) {
                __asm__ __volatile__("pause" ::: "memory");
                continue;
            }
            comp.CursorMoveTo(mx, my);
            last_move = now;
            prev_x = mx;
            prev_y = my;

            /* Drag/resize tracks the hand; otherwise recompose the slow scene
               at ~30Hz so console output still advances while moving. */
            if (dragging || resizing || now - last_scene >= scene_gap) {
                comp.Compose();
                last_scene = now;
            }
        } else {
            /* Pointer still: poll on a short bounded spin instead of a long
               sys_yield(). A yield can be scheduled out longer than a quick
               button press, which would make us miss the press edge and drop
               the click entirely; a 2 ms cap guarantees the down/up edges are
               always sampled. Fairness against other tasks still comes from
               the scheduler's preemptive tick (pause is HT-friendly). */
            if (now - last_scene < idle_gap) {
                uint64_t until = now + 2u * tsc_per_ms;
                do {
                    __asm__ __volatile__("pause" ::: "memory");
                } while (rdtsc64() < until);
                continue;
            }
            comp.SetCursor(mx, my, true);
            comp.Compose();
            last_scene = now;
            last_move  = now;
            prev_x = mx;
            prev_y = my;
        }
    }
    syscall(9, 0, 0, 0, 0, 0, 0);
    return 0;
}
