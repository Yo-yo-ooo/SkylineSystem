//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

// synthesizer/wm_client.cpp — generic windowed-client management.
#include <synthesizer/wm_client.h>
#include <graphic/fb.h>
#include <syscall.h>
#include <stdlib.h>
#include <string.h>

/* Generic windowed-app spawn (defined in loader.cpp). */
uint64_t SpawnWindowedApp(FrameBuffer* Fb, const char* elf, const char* title,
                          uint32_t bodyW, uint32_t bodyH,
                          SkyWinPlacement* place, uint32_t paperRGB);

/* Rounded/shadowed chrome rasterizer (defined in loader.cpp). */
void SkyPaintChromeSized(FrameBuffer* s, int32_t surfW, int32_t surfH,
                         int32_t bodyW, int32_t bodyH, int32_t restoreGlyph,
                         const char* title, uint32_t paperRGB);

/* One shared lightweight preview surface, used for the duration of a resize
   drag. Only one window can be resized at a time, so one buffer is enough;
   it is allocated lazily and sized for the whole work area. */
static uint32_t* g_rzPreview = nullptr;
static int32_t   g_rzPitch   = 0;
static int32_t   g_rzRows    = 0;

static bool rz_prepare(int32_t pitch, int32_t rows) {
    if (g_rzPreview && g_rzPitch >= pitch && g_rzRows >= rows) return true;
    uint32_t* p = (uint32_t*)malloc((size_t)pitch * (size_t)rows * sizeof(uint32_t));
    if (!p) return false;
    free(g_rzPreview);
    g_rzPreview = p;
    g_rzPitch   = pitch;
    g_rzRows    = rows;
    return true;
}

static bool rz_client_surface(WmClient* c, int32_t pitch, int32_t rows) {
    if (c->rzSurf && c->rzPitch >= pitch) return true;
    uint32_t* p = (uint32_t*)malloc((size_t)pitch * (size_t)rows * sizeof(uint32_t));
    if (!p) return false;
    free(c->rzSurf);
    c->rzSurf  = p;
    c->rzPitch = pitch;
    return true;
}

/* Mirror the client's fixed bitmap 1:1 into a WM-owned presentation surface.
   No scaling: a smaller body crops, a larger one leaves the chrome paper. */
static void rz_mirror(uint32_t* dst, int32_t dstPitch, int32_t dx, int32_t dy,
                      int32_t bodyW, int32_t bodyH, const WmClient* c) {
    const int32_t srcPitch   = (int32_t)c->place.w;      /* spawn surface width */
    const int32_t srcContentW = (int32_t)(c->place.w - 2u * SKYWIN_SHADOW);
    const int32_t srcContentH = (int32_t)(c->place.h - 2u * SKYWIN_SHADOW)
                                - (int32_t)SKYWIN_TITLE_H - (int32_t)SKYWIN_RADIUS;
    int32_t cw = bodyW, ch = bodyH - (int32_t)SKYWIN_TITLE_H - (int32_t)SKYWIN_RADIUS;
    if (cw > srcContentW) cw = srcContentW;
    if (ch > srcContentH) ch = srcContentH;
    if (cw <= 0 || ch <= 0) return;

    const uint32_t* src = (const uint32_t*)c->place.desk_surf;
    for (int32_t r = 0; r < ch; r++) {
        const uint32_t* sp = src + (uint64_t)(SKYWIN_SHADOW + SKYWIN_TITLE_H + r)
                                   * (uint32_t)srcPitch + SKYWIN_SHADOW;
        uint32_t* dp = dst + (uint64_t)(dy + r) * (uint32_t)dstPitch + dx;
        memcpy(dp, sp, (size_t)cw * sizeof(uint32_t));
    }
}

/* Paint the full (SDF, shadowed) chrome for a resized client. */
static void rz_paint_full(WmClient* c, int32_t pitch, int32_t nw, int32_t nh) {
    const int32_t rows = nh + 2 * (int32_t)SKYWIN_SHADOW;
    memset(c->rzSurf, 0, (size_t)pitch * (size_t)rows * sizeof(uint32_t));
    FrameBuffer rb;
    rb.BaseAddress       = c->rzSurf;
    rb.BufferSize        = (uint64_t)pitch * rows * sizeof(uint32_t);
    rb.Width = rb.PixelsPerScanLine = (uint64_t)pitch;
    rb.Height            = (uint64_t)rows;
    SkyPaintChromeSized(&rb, pitch, rows, nw, nh, 0, c->title ? c->title : "",
                        c->paperRGB);
    rz_mirror(c->rzSurf, pitch, (int32_t)SKYWIN_SHADOW,
              (int32_t)SKYWIN_SHADOW + (int32_t)SKYWIN_TITLE_H, nw, nh, c);
}

/* Cheap LIVE preview: flat fills and a 1px rim only — no SDF, no soft shadow,
   so an emulated core keeps up with the PS/2 packet rate and the drag tracks
   the hand (same reasoning as the console's live resize path). */
static void rz_paint_live(uint32_t* dst, int32_t pitch, int32_t nw, int32_t nh,
                          const WmClient* c) {
    const int32_t M = (int32_t)SKYWIN_SHADOW;
    const int32_t rows = nh + 2 * M;
    memset(dst, 0, (size_t)pitch * (size_t)rows * sizeof(uint32_t));

    const uint32_t face  = SKYRGB_TITLE;
    const uint32_t rim   = SKYRGB_BORDER;
    for (int32_t y = M; y < M + nh; y++) {
        uint32_t* row = dst + (uint64_t)y * (uint32_t)pitch;
        for (int32_t x = M; x < M + nw; x++)
            row[x] = 0xFF000000u | (face & 0x00FFFFFFu);
    }
    /* 1px rim */
    for (int32_t x = M; x < M + nw; x++) {
        dst[(uint64_t)M * (uint32_t)pitch + x]                 = 0xFF000000u | (rim & 0x00FFFFFFu);
        dst[(uint64_t)(M + nh - 1) * (uint32_t)pitch + x]      = 0xFF000000u | (rim & 0x00FFFFFFu);
    }
    for (int32_t y = M; y < M + nh; y++) {
        dst[(uint64_t)y * (uint32_t)pitch + M]                 = 0xFF000000u | (rim & 0x00FFFFFFu);
        dst[(uint64_t)y * (uint32_t)pitch + M + nw - 1]        = 0xFF000000u | (rim & 0x00FFFFFFu);
    }
    /* caption strip + content paper */
    for (int32_t y = M; y < M + (int32_t)SKYWIN_TITLE_H && y < M + nh; y++) {
        uint32_t* row = dst + (uint64_t)y * (uint32_t)pitch;
        for (int32_t x = M; x < M + nw; x++)
            row[x] = 0xFF000000u | (SKYRGB_TITLE & 0x00FFFFFFu);
    }
    rz_mirror(dst, pitch, M, M + (int32_t)SKYWIN_TITLE_H, nw, nh, c);
}

/* Apply a body geometry to the compositor window. When the body is back at
   its spawn size the client's own shared surface is used again (1:1, no copy);
   otherwise the window is presented from the WM-owned resize surface. */
static void rz_apply(WmClient* c, int32_t nx, int32_t ny, int32_t nw, int32_t nh,
                     uint32_t* surf, int32_t pitch) {
    const int32_t M = (int32_t)SKYWIN_SHADOW;
    c->bodyX = nx; c->bodyY = ny;
    c->bodyW = (uint32_t)nw; c->bodyH = (uint32_t)nh;
    c->win.PosX = nx - M;
    c->win.PosY = ny - M;
    if (surf) {
        c->win.SizeX = (uint32_t)pitch;
        c->win.SizeY = (uint32_t)(nh + 2 * M);
        c->win.FbAddr = (uint64_t)surf;
    } else {
        c->win.SizeX = c->place.w;
        c->win.SizeY = c->place.h;
        c->win.FbAddr = c->place.desk_surf;
    }
    c->win.FrameStartX = (uint32_t)M;
    c->win.FrameStartY = (uint32_t)(M + SKYWIN_TITLE_H);
    c->win.FrameEndX   = (uint32_t)(M + nw);
    c->win.FrameEndY   = (uint32_t)(M + nh - SKYWIN_RADIUS);
    c->win.HasAlpha    = 1;
}

/* Slot table: Window / SkyWinPlacement addresses must stay stable once the
   compositor references them, so the registry is static storage (no realloc,
   no move). WM_MAX_CLIENTS bounds it; raise it to run more apps at once. */
#define WM_MAX_CLIENTS 8
static WmClient g_clients[WM_MAX_CLIENTS];

/* The client that owns the current mouse press (nullptr = none). */
static WmClient* g_press = nullptr;

/* Visible body rect of a client: the shared surface minus the shadow margin
   (the soft shadow is part of the surface but must not be clickable).
   Body geometry is authoritative — after a resize the presented surface is
   wider than the body, so deriving it from win.SizeX would be wrong. */
static void wm_body(const WmClient* c, int32_t* x, int32_t* y,
                    int32_t* w, int32_t* h) {
    *x = c->bodyX;
    *y = c->bodyY;
    *w = (int32_t)c->bodyW;
    *h = (int32_t)c->bodyH;
}

/* Drop a client's window without touching the process: unregister (the
   auto-created layer is retired with its last window and freed at the next
   Compose() barrier) and free the slot. Callers must null any handle. */
static void wm_drop(Compositor& comp, WmClient* c) {
    comp.UnregisterWindow(&c->win);
    free(c->rzSurf);
    c->rzSurf = nullptr;
    c->rzPitch = 0;
    c->place.client_pid = 0;
    c->layer = nullptr;          /* retired/freed by the compositor */
    c->used = false;
    c->dragging = false;
    c->resizing = false;
    c->pressClose = false;
    c->pressMin = false;
    c->minimized = false;
    if (g_press == c) g_press = nullptr;
}

void wm_client_close(Compositor& comp, WmClient* c) {
    if (!c || !c->used) return;
    if (c->place.client_pid) sys_kill(c->place.client_pid, 9);
    wm_drop(comp, c);
}

void wm_client_minimize(Compositor& comp, WmClient* c) {
    if (!c || !c->used) return;
    c->minimized = true;
    comp.SetVisible(&c->win, false);
}

void wm_client_restore(Compositor& comp, WmClient* c) {
    if (!c || !c->used || !c->layer) return;
    c->minimized = false;
    comp.SetVisible(&c->win, true);
    comp.RaiseWindow(&c->win);   /* a restored window takes the top */
}

void wm_client_toggle_max(WmClient* c, int32_t scrW, int32_t scrH,
                          int32_t taskbarH, bool* dirty) {
    if (!c || !c->used || !c->layer) return;
    const int32_t M = (int32_t)SKYWIN_SHADOW;
    if (!c->maximized) {
        c->savedX = c->bodyX; c->savedY = c->bodyY;
        c->savedW = c->bodyW; c->savedH = c->bodyH;
        int32_t nw = scrW - 2 * M;
        int32_t nh = (scrH - taskbarH) - 2 * M;
        if (nw < (int32_t)SKYWIN_MIN_W) nw = (int32_t)SKYWIN_MIN_W;
        if (nh < (int32_t)SKYWIN_MIN_H) nh = (int32_t)SKYWIN_MIN_H;
        const int32_t pitch = scrW + 2 * M;
        const int32_t rows  = (scrH - taskbarH) + 2 * M;
        if (!rz_client_surface(c, pitch, rows)) return;
        c->maximized = true;
        rz_paint_full(c, c->rzPitch, nw, nh);
        rz_apply(c, M, M, nw, nh, c->rzSurf, c->rzPitch);
    } else {
        c->maximized = false;
        rz_apply(c, c->savedX, c->savedY, (int32_t)c->savedW, (int32_t)c->savedH,
                 nullptr, 0);
    }
    if (dirty) *dirty = true;
}

int wm_client_count(void) {
    int n = 0;
    for (int i = 0; i < WM_MAX_CLIENTS; i++) if (g_clients[i].used) n++;
    return n;
}

WmClient* wm_client_by_order(int idx) {
    int n = 0;
    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        if (!g_clients[i].used) continue;
        if (n++ == idx) return &g_clients[i];
    }
    return nullptr;
}

WmClient* wm_client_top_visible(void) {
    WmClient* best = nullptr;
    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        WmClient* c = &g_clients[i];
        if (!c->used || !c->layer || c->minimized) continue;
        if (!best || c->layer->z > best->layer->z) best = c;
    }
    return best;
}

WmClient* wm_client_spawn(Compositor& comp, FrameBuffer* fb, const char* elf,
                          const char* title, uint32_t bodyW, uint32_t bodyH,
                          uint32_t paperRGB, uint32_t dx, uint32_t dy) {
    WmClient* c = nullptr;
    for (int i = 0; i < WM_MAX_CLIENTS; i++)
        if (!g_clients[i].used) { c = &g_clients[i]; break; }
    if (!c) return nullptr;

    memset(c, 0, sizeof(*c));
    if (!SpawnWindowedApp(fb, elf, title, bodyW, bodyH, &c->place, paperRGB))
        return nullptr;
    if (!c->place.desk_surf) {
        if (c->place.client_pid) sys_kill(c->place.client_pid, 9);
        return nullptr;
    }

    c->used     = true;
    c->title    = title;
    c->paperRGB = paperRGB;
    c->bodyW    = bodyW;
    c->bodyH    = bodyH;

    /* Presentation surface = the whole shared surface, offset from the
       spawn placement so stacked windows stay visible. Use rz_apply() so the
       body geometry and the window rect can never drift apart (resize and
       drag both go through the same helper). */
    rz_apply(c, (int32_t)c->place.x + (int32_t)dx,
             (int32_t)c->place.y + (int32_t)dy,
             (int32_t)bodyW, (int32_t)bodyH, nullptr, 0);

    c->layer = comp.RegisterWindowAuto(&c->win);
    if (!c->layer) {
        if (c->place.client_pid) sys_kill(c->place.client_pid, 9);
        c->used = false;
        return nullptr;
    }
    return c;
}

WmClient* wm_client_hit(int32_t x, int32_t y) {
    WmClient* best = nullptr;
    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        WmClient* c = &g_clients[i];
        if (!c->used || !c->layer) continue;
        int32_t bx, by, bw, bh;
        wm_body(c, &bx, &by, &bw, &bh);
        if (x >= bx && x < bx + bw && y >= by && y < by + bh)
            if (!best || c->layer->z > best->layer->z) best = c;
    }
    return best;
}

/* True while a generic client owns the current mouse press (wm_client_press
   ran on the press edge) — used by the WM so the taskbar hit test does not
   steal a press that already landed on a client window body. */
bool wm_client_pressed(void) { return g_press != nullptr; }

void wm_client_press(Compositor& comp, WmClient* c, int32_t mx, int32_t my) {
    if (!c || !c->used) return;
    g_press = c;
    comp.RaiseWindow(&c->win);            /* click-to-front (Win11) */

    int32_t bx, by, bw, bh;
    wm_body(c, &bx, &by, &bw, &bh);
    const int32_t th = (int32_t)SKYWIN_TITLE_H;
    int32_t btnStart = bx + bw - 3 * (int32_t)SKYWIN_BTN_W;
    int32_t closeL   = bx + bw - (int32_t)SKYWIN_BTN_W;
    int32_t maxL     = bx + bw - 2 * (int32_t)SKYWIN_BTN_W;
    if (mx >= closeL && mx < closeL + (int32_t)SKYWIN_BTN_W &&
        my >= by && my < by + th) {
        c->pressClose = true;             /* close button armed            */
    } else if (mx >= maxL && mx < maxL + (int32_t)SKYWIN_BTN_W &&
               my >= by && my < by + th) {
        c->pressMax = true;               /* maximize button armed         */
    } else if (mx >= btnStart && mx < btnStart + (int32_t)SKYWIN_BTN_W &&
               my >= by && my < by + th) {
        c->pressMin = true;               /* minimize button armed         */
    } else if (my < by + th && mx < btnStart) {
        c->dragging  = true;              /* caption drag armed            */
        c->grabX = mx - (int32_t)c->win.PosX;
        c->grabY = my - (int32_t)c->win.PosY;
    }
    /* A press anywhere else on the body only raised the window. */
}

/* ---- interactive edge/corner resize ------------------------------------
   Mirrors the console's policy: an 8px band (plus a small outside margin so
   corners are grabbable over the shadow) around the topmost client body. */
WmClient* wm_client_edge_at(int32_t x, int32_t y, uint8_t* dir_out) {
    WmClient* best = nullptr;
    uint8_t   bestDir = 0;
    const int32_t OUT = 4;
    const int32_t RB  = (int32_t)SKYWIN_RESIZE_BORDER;

    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        WmClient* c = &g_clients[i];
        if (!c->used || !c->layer || c->minimized) continue;
        int32_t bx, by, bw, bh;
        wm_body(c, &bx, &by, &bw, &bh);
        bool spanX = x >= bx - OUT && x < bx + bw + OUT;
        bool spanY = y >= by - OUT && y < by + bh + OUT;
        int32_t dL = x - bx, dR = bx + bw - 1 - x;
        int32_t dT = y - by, dB = by + bh - 1 - y;
        bool onL = spanY && dL >= -OUT && dL < RB;
        bool onR = spanY && dR >= -OUT && dR < RB;
        bool onT = spanX && dT >= -OUT && dT < RB;
        bool onB = spanX && dB >= -OUT && dB < RB;
        if (!onL && !onR && !onT && !onB) continue;
        uint8_t dir = (uint8_t)((onL ? 1 : 0) | (onR ? 2 : 0) |
                                (onT ? 4 : 0) | (onB ? 8 : 0));
        if (!best || c->layer->z > best->layer->z) { best = c; bestDir = dir; }
    }
    if (!best) return nullptr;
    if (dir_out) *dir_out = bestDir;
    return best;
}

void wm_client_resize_begin(WmClient* c, int32_t mx, int32_t my, uint8_t dir) {
    if (!c || !c->used) return;
    c->resizing = true;
    c->maximized = false;    /* an explicit resize leaves the maximized state */
    c->rzDir    = dir;
    c->rsX = c->bodyX; c->rsY = c->bodyY;
    c->rsW = (int32_t)c->bodyW; c->rsH = (int32_t)c->bodyH;
    c->rsMX = mx; c->rsMY = my;
}

void wm_client_resize_drag(WmClient* c, int32_t mx, int32_t my,
                           int32_t scrW, int32_t scrH, int32_t taskbarH,
                           bool* dirty) {
    if (!c || !c->used || !c->resizing) return;
    const int32_t M     = (int32_t)SKYWIN_SHADOW;
    const int32_t maxW  = scrW;
    const int32_t maxH  = scrH - taskbarH;
    const int32_t minW  = (int32_t)SKYWIN_MIN_W;
    const int32_t minH  = (int32_t)SKYWIN_MIN_H;

    int32_t dx = mx - c->rsMX, dy = my - c->rsMY;
    int32_t nw = c->rsW, nh = c->rsH, nx = c->rsX, ny = c->rsY;
    if (c->rzDir & 2) nw = c->rsW + dx;                     /* right  */
    if (c->rzDir & 8) nh = c->rsH + dy;                     /* bottom */
    if (c->rzDir & 1) { nw = c->rsW - dx; nx = c->rsX + dx; }
    if (c->rzDir & 4) { nh = c->rsH - dy; ny = c->rsY + dy; }
    if (nw < minW) { if (c->rzDir & 1) nx -= (minW - nw); nw = minW; }
    if (nh < minH) { if (c->rzDir & 4) ny -= (minH - nh); nh = minH; }
    if (nw > maxW) nw = maxW;
    if (nh > maxH) nh = maxH;
    if (nx < -M) { nw += (nx + M); nx = -M; if (nw < minW) nw = minW; }
    if (ny < -M) { nh += (ny + M); ny = -M; if (nh < minH) nh = minH; }

    /* back at the spawn size -> present the client's own surface again */
    if (nw == (int32_t)(c->place.w - 2u * M) &&
        nh == (int32_t)(c->place.h - 2u * M)) {
        rz_apply(c, nx, ny, nw, nh, nullptr, 0);
        if (dirty) *dirty = true;
        return;
    }

    const int32_t pitch = maxW + 2 * M;
    const int32_t rows  = maxH + 2 * M;
    if (!rz_prepare(pitch, rows)) return;
    rz_paint_live(g_rzPreview, pitch, nw, nh, c);
    rz_apply(c, nx, ny, nw, nh, g_rzPreview, pitch);
    if (dirty) *dirty = true;
}

void wm_client_resize_end(WmClient* c, int32_t scrW, int32_t scrH,
                          int32_t taskbarH, bool* dirty) {
    if (!c || !c->used || !c->resizing) return;
    c->resizing = false;
    const int32_t M = (int32_t)SKYWIN_SHADOW;
    if (c->bodyW == (c->place.w - 2u * M) && c->bodyH == (c->place.h - 2u * M)) {
        rz_apply(c, c->bodyX, c->bodyY, (int32_t)c->bodyW, (int32_t)c->bodyH,
                 nullptr, 0);
        if (dirty) *dirty = true;
        return;
    }
    /* Full chrome (SDF + shadow) exactly once, at the final geometry. The
       buffer is sized for the whole work area so later drags never realloc. */
    const int32_t pitch = scrW + 2 * M;
    const int32_t rows  = (scrH - taskbarH) + 2 * M;
    if (!rz_client_surface(c, pitch, rows)) return;
    rz_paint_full(c, c->rzPitch, (int32_t)c->bodyW, (int32_t)c->bodyH);
    rz_apply(c, c->bodyX, c->bodyY, (int32_t)c->bodyW, (int32_t)c->bodyH,
             c->rzSurf, c->rzPitch);
    if (dirty) *dirty = true;
}

bool wm_client_any_resize(void) {
    for (int i = 0; i < WM_MAX_CLIENTS; i++)
        if (g_clients[i].used && g_clients[i].resizing) return true;
    return false;
}

void wm_client_release(Compositor& comp, int32_t mx, int32_t my,
                       int32_t scrW, int32_t scrH, int32_t taskbarH,
                       bool* dirty) {
    WmClient* c = g_press;
    g_press = nullptr;

    if (c && c->used && c->pressClose) {
        /* fire only if the release is still inside the close button — the
           same press-and-release-inside rule the console buttons use. */
        int32_t bx, by, bw, bh;
        wm_body(c, &bx, &by, &bw, &bh);
        int32_t closeL = bx + bw - (int32_t)SKYWIN_BTN_W;
        if (mx >= closeL && mx < closeL + (int32_t)SKYWIN_BTN_W &&
            my >= by && my < by + (int32_t)SKYWIN_TITLE_H)
            wm_client_close(comp, c);
    }

    if (c && c->used && c->pressMin) {
        /* minimize fires with the same release-inside rule. */
        int32_t bx, by, bw, bh;
        wm_body(c, &bx, &by, &bw, &bh);
        int32_t minL = bx + bw - 3 * (int32_t)SKYWIN_BTN_W;
        if (mx >= minL && mx < minL + (int32_t)SKYWIN_BTN_W &&
            my >= by && my < by + (int32_t)SKYWIN_TITLE_H)
            wm_client_minimize(comp, c);
    }

    if (c && c->used && c->pressMax) {
        /* maximize fires with the same release-inside rule. */
        int32_t bx, by, bw, bh;
        wm_body(c, &bx, &by, &bw, &bh);
        int32_t maxL = bx + bw - 2 * (int32_t)SKYWIN_BTN_W;
        if (mx >= maxL && mx < maxL + (int32_t)SKYWIN_BTN_W &&
            my >= by && my < by + (int32_t)SKYWIN_TITLE_H) {
            bool d = false;
            wm_client_toggle_max(c, scrW, scrH, taskbarH, &d);
            if (d && dirty) *dirty = true;
        }
    }

    /* a release edge ends every armed drag / armed button, client or not */
    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        g_clients[i].dragging   = false;
        g_clients[i].pressClose = false;
        g_clients[i].pressMin   = false;
        g_clients[i].pressMax   = false;
    }
}

void wm_client_drag_all(Compositor& comp, int32_t mx, int32_t my,
                        int32_t scrW, int32_t scrH, int32_t taskbarH) {
    const int32_t m  = (int32_t)SKYWIN_SHADOW;
    const int32_t th = (int32_t)SKYWIN_TITLE_H;
    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        WmClient* c = &g_clients[i];
        if (!c->used || !c->dragging) continue;
        int32_t nx = mx - c->grabX, ny = my - c->grabY;   /* surface top-left */
        int32_t bodyW = (int32_t)c->bodyW;
        /* keep >=120px of the body on screen and above the taskbar (the
           same policy the console caption drag uses) */
        const int32_t xLo = -(bodyW + 2 * m - 120);
        const int32_t xHi = scrW - 120;
        const int32_t yLo = -m;
        const int32_t yHi = scrH - taskbarH - th - m;
        if (nx < xLo) nx = xLo;
        if (nx > xHi) nx = xHi;
        if (ny < yLo) ny = yLo;
        if (ny > yHi) ny = yHi;
        /* keep presenting from whatever surface the window currently uses
           (own shared surface, or the WM resize surface after a resize) */
        uint32_t* surf = c->rzSurf;
        int32_t   pitch = c->rzPitch;
        if (c->bodyW == (c->place.w - 2u * m) &&
            c->bodyH == (c->place.h - 2u * m)) { surf = nullptr; pitch = 0; }
        rz_apply(c, nx + m, ny + m, (int32_t)c->bodyW, (int32_t)c->bodyH,
                 surf, pitch);
        comp.MoveWindow(&c->win, c->win.PosX, c->win.PosY);
    }
}

bool wm_client_any_drag(void) {
    for (int i = 0; i < WM_MAX_CLIENTS; i++)
        if (g_clients[i].used && g_clients[i].dragging) return true;
    return false;
}

void wm_clients_sweep(Compositor& comp, bool* dirty) {
    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        WmClient* c = &g_clients[i];
        if (!c->used || !c->place.client_pid) continue;
        /* sys_kill returns uint64_t with errors encoded as negative values;
           cast before comparing (an unsigned "< 0" is always false). */
        if ((int64_t)sys_kill(c->place.client_pid, 0) < 0) { /* liveness probe */
            wm_drop(comp, c);
            if (dirty) *dirty = true;
        }
    }
}

void wm_clients_poll_output(bool* dirty) {
    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        WmClient* c = &g_clients[i];
        if (!c->used || !c->place.proto) continue;
        uint64_t seq = *(volatile uint64_t*)(c->place.proto +
                                             SKYWIN_PROTO_OUT_SEQ * 8u);
        if (seq == c->last_seq) continue;
        c->last_seq = seq;
        if (dirty) *dirty = true;
    }
}