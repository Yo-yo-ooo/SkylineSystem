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

/* Slot table: Window / SkyWinPlacement addresses must stay stable once the
   compositor references them, so the registry is static storage (no realloc,
   no move). WM_MAX_CLIENTS bounds it; raise it to run more apps at once. */
#define WM_MAX_CLIENTS 8
static WmClient g_clients[WM_MAX_CLIENTS];

/* The client that owns the current mouse press (nullptr = none). */
static WmClient* g_press = nullptr;

/* Visible body rect of a client: the shared surface minus the shadow margin
   (the soft shadow is part of the surface but must not be clickable). */
static void wm_body(const WmClient* c, int32_t* x, int32_t* y,
                    int32_t* w, int32_t* h) {
    const int32_t m = (int32_t)SKYWIN_SHADOW;
    *x = (int32_t)c->win.PosX + m;
    *y = (int32_t)c->win.PosY + m;
    *w = (int32_t)c->win.SizeX - 2 * m;
    *h = (int32_t)c->win.SizeY - 2 * m;
}

/* Drop a client's window without touching the process: unregister (the
   auto-created layer is retired with its last window and freed at the next
   Compose() barrier) and free the slot. Callers must null any handle. */
static void wm_drop(Compositor& comp, WmClient* c) {
    comp.UnregisterWindow(&c->win);
    c->place.client_pid = 0;
    c->layer = nullptr;          /* retired/freed by the compositor */
    c->used = false;
    c->dragging = false;
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

    c->used  = true;
    c->title = title;

    /* Presentation surface = the whole shared surface, offset from the
       spawn placement so stacked windows stay visible. */
    c->win.PosX = c->place.x + dx;
    c->win.PosY = c->place.y + dy;
    c->win.SizeX = c->place.w;
    c->win.SizeY = c->place.h;
    /* Client-content frame inside the surface (same convention as before):
       below the title bar, above the bottom rounded corner. */
    c->win.FrameStartX = SKYWIN_SHADOW;
    c->win.FrameStartY = SKYWIN_SHADOW + SKYWIN_TITLE_H;
    c->win.FrameEndX   = SKYWIN_SHADOW + bodyW;
    c->win.FrameEndY   = SKYWIN_SHADOW + bodyH - SKYWIN_RADIUS;
    c->win.FbAddr   = c->place.desk_surf;
    c->win.HasAlpha = 1;         /* rounded corners + soft drop shadow */

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
    if (mx >= closeL && mx < closeL + (int32_t)SKYWIN_BTN_W &&
        my >= by && my < by + th) {
        c->pressClose = true;             /* close button armed            */
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

void wm_client_release(Compositor& comp, int32_t mx, int32_t my) {
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

    /* a release edge ends every armed drag / armed button, client or not */
    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        g_clients[i].dragging   = false;
        g_clients[i].pressClose = false;
        g_clients[i].pressMin   = false;
    }
}

void wm_client_drag_all(Compositor& comp, int32_t mx, int32_t my,
                        int32_t scrW, int32_t scrH, int32_t taskbarH) {
    const int32_t m  = (int32_t)SKYWIN_SHADOW;
    const int32_t th = (int32_t)SKYWIN_TITLE_H;
    for (int i = 0; i < WM_MAX_CLIENTS; i++) {
        WmClient* c = &g_clients[i];
        if (!c->used || !c->dragging) continue;
        int32_t nx = mx - c->grabX, ny = my - c->grabY;
        int32_t bodyW = (int32_t)c->win.SizeX - 2 * m;
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
        c->win.PosX = (uint32_t)nx;
        c->win.PosY = (uint32_t)ny;
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