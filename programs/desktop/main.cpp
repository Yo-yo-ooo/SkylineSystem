//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <graphic/fb.h>
#include <syscall.h>
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

/* Caption glyph painter shared with loader.cpp (normal + maximized chrome). */
void SkyPaintCaptionIcons(FrameBuffer* s, int32_t bx0, int32_t by0,
                          int32_t bodyW, int32_t titleH, int maximized);

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

/* ---- monotous TSC frame-pacing helpers -----------------------------------
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
/*  Window-manager helpers: taskbar, normal/maximized geometry, content mirror */
/* ========================================================================== */

/* Paint the bottom taskbar and its single console app-button straight into
   the wallpaper bitmap (compositor layer 0). `minimized` selects the active
   "click to restore" look with a Fluent accent edge. */
static void wm_draw_taskbar(uint32_t* wall, uint32_t W, uint32_t H, bool minimized) {
    FrameBuffer lb;
    lb.BaseAddress       = wall;
    lb.BufferSize        = (uint64_t)W * H * sizeof(uint32_t);
    lb.Width = lb.PixelsPerScanLine = W;
    lb.Height            = H;

    const uint32_t barH = SKYWIN_TASKBAR_H;
    const uint32_t y0   = H - barH;
    DrawFillRect(&lb, 0, y0, W, barH, SKYRGB_TASKBAR);
    DrawFillRect(&lb, 0, y0, W, 1, SKYRGB_SEP);

    const uint32_t bx = 8, by = y0 + 6, bw = 200, bh = barH - 12;
    DrawFillRect(&lb, bx, by, bw, bh, minimized ? SKYRGB_TBTN_ON : SKYRGB_TBTN_IDLE);
    DrawRect(&lb, bx, by, bw, bh, minimized ? SKYRGB_ACCENT : SKYRGB_BORDER);
    if (minimized) DrawFillRect(&lb, bx, by, 3, bh, SKYRGB_ACCENT);

    TTF_Font* f = console_font();
    if (f)
        TTF_DrawText(&lb, f, (int32_t)bx + 14, (int32_t)(by + (bh - 22) / 2),
                     "Skyline Console", minimized ? 0xFFFFFFFFu : SKYRGB_INK);
}

/* Switch the console window to its fixed rounded NORMAL surface. Called only
   between synchronous Compose() frames, when the workers are parked on the
   barrier and cannot observe a half-updated Window. */
static void wm_apply_normal(Window* w, const SkyWinPlacement* pl,
                           uint32_t nx, uint32_t ny) {
    w->PosX = nx;  w->PosY = ny;
    w->SizeX = SKYWIN_SURF_W;  w->SizeY = SKYWIN_SURF_H;
    w->FbAddr = pl->desk_surf;
    w->HasAlpha = 1;
}

/* Switch the console window to the opaque, full-work-area MAXIMIZED surface. */
static void wm_apply_max(Window* w, const uint32_t* maxSurf,
                        uint32_t mw, uint32_t mh) {
    w->PosX = 0;  w->PosY = 0;
    w->SizeX = mw;  w->SizeY = mh;
    w->FbAddr = (uint64_t)maxSurf;
    w->HasAlpha = 0;
}

/* While maximized, mirror the client's latest terminal pixels (it keeps
   writing its fixed NORMAL shared surface) 1:1 into the maximized client area.
   No scaling, so glyphs stay crisp; only CONTENT_W x CONTENT_H pixels move. */
static void wm_sync_max_content(uint32_t* maxSurf, uint32_t maxW,
                                const uint32_t* normSurf) {
    for (uint32_t r = 0; r < SKYWIN_CONTENT_H; r++) {
        const uint32_t* sp = normSurf
            + (uint64_t)(SKYWIN_CONTENT_Y + r) * SKYWIN_SURF_W + SKYWIN_CONTENT_X;
        uint32_t* dp = maxSurf + (uint64_t)(SKYWIN_TITLE_H + r) * maxW;
        memcpy(dp, sp, (size_t)SKYWIN_CONTENT_W * sizeof(uint32_t));
    }
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

    BasicDraw bd((FrameBuffer*)&fb);
    bd.RenderWallpaper(wallBuf);
    wm_draw_taskbar(wallBuf, scrW, scrH, false);   /* restore entry on layer 0 */

    Compositor& comp = Compositor::Get();
    if (!comp.Init((FrameBuffer*)&fb)) return 1;

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
    CompLayer* layer1 = nullptr;
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
        layer1 = comp.CreateLayer(1);
        comp.RegisterWindow(&consoleWin, layer1);
    }

    /* Desktop-owned (NOT shared with the client) full-work-area surface used
       only while maximized: opaque, no rounded shadow margin. The live text is
       mirrored in from the normal shared surface each frame (wm_sync). */
    const uint32_t maxW = scrW;
    const uint32_t maxH = scrH - SKYWIN_TASKBAR_H;
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

    int32_t prev_x = -100;
    int32_t prev_y = -100;

    int32_t fb_width = (int32_t)fb.Width;
    int32_t fb_height = (int32_t)fb.Height;

    ps2_mouse_state_t *p = (ps2_mouse_state_t*)mouse_addr;

    uint32_t seq1, seq2;
    int32_t mx, my;
    uint64_t last_move  = rdtsc64();
    uint64_t last_scene = rdtsc64();

    /* ---- window-manager interaction state ---- */
    enum { WM_NORMAL = 0, WM_MAX = 1, WM_MIN = 2 } wmMode = WM_NORMAL;
    uint32_t normX = place.x, normY = place.y;  /* normal surface top-left    */
    bool     prevLeft = false, dragging = false;
    int32_t  grabDX = 0, grabDY = 0;
    int      pressHit = 0;   /* 0 none,1 caption,2 min,3 max,4 close,5 taskbar */
    uint8_t  ml = 0;         /* left-button snapshot from the seqlock block    */

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

        /* Current BODY rectangle on screen (the visible rect; the normal
           surface adds the SKYWIN_SHADOW margin around it). */
        const int32_t M  = (int32_t)SKYWIN_SHADOW;
        const int32_t th = (int32_t)SKYWIN_TITLE_H;
        int32_t bx, by, bw;
        if (wmMode == WM_MAX) { bx = 0; by = 0; bw = (int32_t)maxW; }
        else { bx = (int32_t)normX + M; by = (int32_t)normY + M; bw = (int32_t)SKYWIN_W; }

        /* taskbar app-button hit rectangle (fixed, on layer 0) */
        const int32_t tbX0 = 8, tbW = 200;
        const int32_t tbY0 = fb_height - (int32_t)SKYWIN_TASKBAR_H + 6;
        const int32_t tbH  = (int32_t)SKYWIN_TASKBAR_H - 12;
        auto inBox = [&](int32_t x0, int32_t y0, int32_t ww, int32_t hh) {
            return mx >= x0 && mx < x0 + ww && my >= y0 && my < y0 + hh;
        };
        bool wmDirty = false;   /* a state change happened -> repaint now     */

        if (leftDown && !prevLeft) {                 /* press edge: classify  */
            pressHit = 0;
            if (wmMode != WM_MIN) {
                int32_t minL   = bx + bw - 3 * (int32_t)SKYWIN_BTN_W;
                int32_t maxL   = bx + bw - 2 * (int32_t)SKYWIN_BTN_W;
                int32_t closeL = bx + bw - 1 * (int32_t)SKYWIN_BTN_W;
                if      (inBox(closeL, by, (int32_t)SKYWIN_BTN_W, th)) pressHit = 4;
                else if (inBox(maxL,   by, (int32_t)SKYWIN_BTN_W, th)) pressHit = 3;
                else if (inBox(minL,   by, (int32_t)SKYWIN_BTN_W, th)) pressHit = 2;
                else if (wmMode == WM_NORMAL && inBox(bx, by, bw, th)) {
                    pressHit = 1;                    /* caption empty area    */
                    dragging = true;
                    grabDX = mx - (int32_t)normX;    /* grab vs SURFACE origin*/
                    grabDY = my - (int32_t)normY;
                }
            }
            if (pressHit == 0 && inBox(tbX0, tbY0, tbW, tbH)) pressHit = 5;
        }

        if (leftDown && dragging && wmMode == WM_NORMAL) {     /* drag move   */
            int32_t nx = mx - grabDX, ny = my - grabDY;
            const int32_t xLo = -((int32_t)SKYWIN_SURF_W - 120);
            const int32_t xHi = fb_width  - 120;
            const int32_t yLo = -M;
            const int32_t yHi = fb_height - (int32_t)SKYWIN_TASKBAR_H - th - M;
            if (nx < xLo) nx = xLo; if (nx > xHi) nx = xHi;
            if (ny < yLo) ny = yLo; if (ny > yHi) ny = yHi;
            normX = (uint32_t)nx; normY = (uint32_t)ny;
            comp.MoveWindow(&consoleWin, normX, normY);
        }

        if (!leftDown && prevLeft) {                 /* release edge: action  */
            bool fire = false;
            if (pressHit >= 2 && pressHit <= 4 && wmMode != WM_MIN) {
                int32_t lx = bx + bw - (5 - pressHit) * (int32_t)SKYWIN_BTN_W;
                fire = inBox(lx, by, (int32_t)SKYWIN_BTN_W, th);
            } else if (pressHit == 5) {
                fire = inBox(tbX0, tbY0, tbW, tbH);
            }
            dragging = false;
            if (fire) {
                if (pressHit == 2 || pressHit == 4) {        /* minimize/close */
                    wmMode = WM_MIN;
                    comp.SetVisible(&consoleWin, false);
                    wm_draw_taskbar(wallBuf, scrW, scrH, true);
                    wmDirty = true;
                } else if (pressHit == 3 && maxSurf) {       /* maximize toggle*/
                    if (wmMode == WM_NORMAL) {
                        wmMode = WM_MAX;
                        wm_apply_max(&consoleWin, maxSurf, maxW, maxH);
                    } else if (wmMode == WM_MAX) {
                        wmMode = WM_NORMAL;
                        wm_apply_normal(&consoleWin, &place, normX, normY);
                    }
                    wmDirty = true;
                } else if (pressHit == 5) {                   /* taskbar toggle */
                    if (wmMode == WM_MIN) {
                        wmMode = WM_NORMAL;
                        wm_apply_normal(&consoleWin, &place, normX, normY);
                        comp.SetVisible(&consoleWin, true);
                        wm_draw_taskbar(wallBuf, scrW, scrH, false);
                    } else {
                        wmMode = WM_MIN;
                        comp.SetVisible(&consoleWin, false);
                        wm_draw_taskbar(wallBuf, scrW, scrH, true);
                    }
                    wmDirty = true;
                }
            }
            pressHit = 0;
        }
        prevLeft = leftDown;

        /* While maximized, mirror the client's latest text before composing. */
        if (wmMode == WM_MAX && maxSurf)
            wm_sync_max_content(maxSurf, maxW, (const uint32_t*)place.desk_surf);
        if (wmDirty) { comp.Compose(); last_scene = rdtsc64(); }

        if (moved) {
            /* Pointer overlay fast path: spin-pace at ~60Hz, then erase and
               re-stamp only the 16x16 square directly on the scanout. */
            if (now - last_move < move_gap) {
                __asm__ __volatile__("pause" ::: "memory");
                continue;
            }
            /* Fast overlay move only (SetCursor is NOT called here: it would
               move the logical target without painting and starve the move).
               CursorMoveTo erases the committed square and stamps the new one. */
            comp.CursorMoveTo(mx, my);
            last_move = now;
            prev_x = mx;
            prev_y = my;

            /* Recompose the underlying scene at ~30Hz so console/window
               output still advances while the pointer is moving; Compose()
               re-stamps the cursor at its current position afterwards. */
            /* Drag the caption at the pointer rate so the window tracks the
               hand 1:1; otherwise recompose the slow-changing scene ~30Hz. */
            if (dragging || now - last_scene >= scene_gap) {
                comp.Compose();
                last_scene = now;
            }
        } else {
            /* Pointer still: refresh the scene (with its cursor overlay) at
               a low rate and yield between tries to spare the core. */
            if (now - last_scene < idle_gap) { sys_yield(); continue; }
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
