//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

// synthesizer/wm_client.h — generic windowed-client management for the WM.
// Every spawned windowed app (notepad, ...) is a WmClient: the compositor
// only sees a Window on an auto-created layer, and ALL interaction
// (click-to-front, caption drag, close button, dead-process cleanup) is
// app-agnostic code in wm_client.cpp. Adding a windowed app to the desktop
// is one wm_client_spawn() call — no per-app logic in the WM loop.
#ifndef SYNTHESIZER_WM_CLIENT_H
#define SYNTHESIZER_WM_CLIENT_H

#include <stdint.h>
#include <synthesizer/window.h>
#include <graphic/winstyle.h>

struct FrameBuffer;                     /* pointer-only use here */

struct WmClient {
    bool            used;        /* slot occupied                              */
    bool            dragging;    /* caption drag in progress                   */
    bool            pressClose;  /* current press is on the close button       */
    bool            pressMin;    /* current press is on the minimize button    */
    bool            minimized;   /* window hidden via the minimize button      */
    int32_t         grabX, grabY;/* press offset inside the surface            */
    CompLayer*      layer;       /* auto layer (nullptr once dropped)          */
    SkyWinPlacement place;       /* shared surface + client pid                */
    Window          win;         /* compositor window (static storage: stable) */
    const char*     title;       /* spawn title (string-literal lifetime)      */
    uint64_t        last_seq;    /* last observed client OUT_SEQ (frame ping)  */
};

WmClient* wm_client_spawn   (Compositor& comp, FrameBuffer* fb, const char* elf,
                             const char* title, uint32_t bodyW, uint32_t bodyH,
                             uint32_t paperRGB, uint32_t dx, uint32_t dy);
WmClient* wm_client_hit     (int32_t x, int32_t y);  /* topmost client body at point */
bool      wm_client_pressed (void);                  /* a client owns the press      */
void      wm_client_press   (Compositor& comp, WmClient* c, int32_t mx, int32_t my);
void      wm_client_release (Compositor& comp, int32_t mx, int32_t my);
void      wm_client_drag_all(Compositor& comp, int32_t mx, int32_t my,
                             int32_t scrW, int32_t scrH, int32_t taskbarH);
bool      wm_client_any_drag(void);
void      wm_clients_sweep  (Compositor& comp, bool* dirty);
/* Per-client frame ping: a client that renders continuously (DOOM) bumps
   OUT_SEQ in its protocol page; poll it every loop turn and mark the scene
   dirty so the compositor picks the new frame up immediately. */
void      wm_clients_poll_output(bool* dirty);
void      wm_client_close   (Compositor& comp, WmClient* c);
void      wm_client_minimize(Compositor& comp, WmClient* c);
void      wm_client_restore (Compositor& comp, WmClient* c);

/* Enumeration used by taskbar drawing / focus routing. */
int       wm_client_count       (void);                 /* used slots              */
WmClient* wm_client_by_order    (int idx);              /* idx-th used client      */
WmClient* wm_client_top_visible (void);                 /* highest-z shown client  */

#endif /* SYNTHESIZER_WM_CLIENT_H */