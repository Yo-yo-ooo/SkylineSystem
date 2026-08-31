//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <graphic/fb.h>
#include <syscall.h>
#include <graphic/basicdraw.hpp>
#include <graphic/winstyle.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <base/font/ttf/ttf.h>
#include <mouse/ps2.h>
#include <synthesizer/window.h>
static char intTo_stringOutput[128];

uint64_t TLoad(FrameBuffer *Fb, SkyWinPlacement *place);

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
        layer1 = comp.CreateLayer(1);
        comp.RegisterWindow(&consoleWin, layer1);
    }

    comp.StartWorkers();

    for (int warm = 0; warm < 8; warm++) {
        comp.Compose();
        sys_yield();
    }

    MouseInit();

    /* Frame pacing: cap active redraws near 60 FPS and merge the burst of
       PS/2 packets a physical move produces into one frame; when the pointer
       is still, refresh at a low rate so console content still updates. */
    const uint64_t tsc_per_ms = probe_tsc_per_ms();
    const uint64_t move_gap   = 16u  * tsc_per_ms;   /* ~60 FPS while moving */
    const uint64_t idle_gap   = 50u  * tsc_per_ms;   /* ~20 FPS when still   */

    int32_t prev_x = -100;
    int32_t prev_y = -100;

    int32_t fb_width = (int32_t)fb.Width;
    int32_t fb_height = (int32_t)fb.Height;

    ps2_mouse_state_t *p = (ps2_mouse_state_t*)mouse_addr;

    uint32_t seq1, seq2;
    int32_t mx, my;
    uint64_t last_frame = rdtsc64();

    comp.SetCursor(0, 0, true);

    for(;;){
        /* take the newest available mouse snapshot (seqlock retry) */
        while (true) {
            seq1 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
            if (seq1 & 1) continue;
            mx = p->x;
            my = p->y;
            seq2 = __atomic_load_n(&p->seq, __ATOMIC_ACQUIRE);
            if (seq1 == seq2) break;
        }

        if (mx < 0) mx = 0;
        if (my < 0) my = 0;
        if (mx >= fb_width - 16) mx = fb_width - 16;
        if (my >= fb_height - 16) my = fb_height - 16;

        uint64_t now = rdtsc64();
        bool moved = (mx != prev_x || my != prev_y);
        if (now - last_frame < (moved ? move_gap : idle_gap)) {
            /* too soon for another frame: yield instead of busy-spinning or
               hammering a full-screen compose for every single packet */
            sys_yield();
            continue;
        }

        /* The pointer is baked into the off-screen frame by the compositor
           and presented with it, so there is never a cursor-less frame. */
        comp.SetCursor(mx, my, true);
        comp.Compose();

        last_frame = now;
        prev_x = mx;
        prev_y = my;
    }
    syscall(9, 0, 0, 0, 0, 0, 0);
    return 0;
}
