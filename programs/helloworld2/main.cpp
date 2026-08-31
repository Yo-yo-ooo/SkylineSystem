//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

// hw2: a userspace console process inside a fixed haribote-style window.
// The parent desktop (TLoad) shares a tightly-packed SKYWIN_W x SKYWIN_H
// surface and publishes geometry at the fixed protocol page 0x400000
// (see graphic/winstyle.h):
//   [0..4] inner text area (consumed by the libc flanterm console),
//   [5..7] the whole window surface used here to paint chrome.
//
// We first paint the raised grey face / blue title bar / close button and
// clear the inner text area, then printf renders glyphs only inside that
// inner area; the desktop compositor presents the shared surface as a
// centered window above the wallpaper.

#include <stdint.h>
#include <stdio.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/fb.h>
#include <graphic/flanterm.h>
#include <graphic/winstyle.h>
#include <base/font/ttf/ttf.h>

/* Draw the classic haribote window chrome onto the whole-window surface. */
static void paint_window(FrameBuffer *fb, TTF_Font *font) {
    const uint32_t W = SKYWIN_W;
    const uint32_t H = SKYWIN_H;
    const uint32_t E = SKYWIN_EDGE;
    const uint32_t T = SKYWIN_TITLE_H;

    // window face (classic haribote grey)
    DrawFillRect(fb, 0, 0, W, H, SKYRGB_FACE);

    // 1px raised 3-D edge: highlight top/left, shadow bottom/right
    DrawFillRect(fb, 0, 0, W, 1, SKYRGB_LIGHT);          // top
    DrawFillRect(fb, 0, 0, 1, H, SKYRGB_LIGHT);          // left
    DrawFillRect(fb, 0, H - 1, W, 1, SKYRGB_SHADOW);     // bottom
    DrawFillRect(fb, W - 1, 0, 1, H, SKYRGB_SHADOW);     // right

    // active (blue) title bar inset by the grey edge
    DrawFillRect(fb, E, E, W - 2u * E, T, SKYRGB_TITLE);

    // title glyph, vertically centered in the bar (console font height 22)
    if (font)
        TTF_DrawText(fb, font, (int32_t)(E + 8), (int32_t)(E + (T - 22u) / 2u),
                     "Skyline Console", SKYRGB_INK);

    // raised close button, vertically centered at the bar's right end
    const uint32_t bs = 20;
    const uint32_t bx = W - E - 8 - bs;
    const uint32_t by = E + (T - bs) / 2;
    DrawFillRect(fb, bx, by, bs, bs, SKYRGB_FACE);
    DrawFillRect(fb, bx, by, bs, 1, SKYRGB_LIGHT);
    DrawFillRect(fb, bx, by, 1, bs, SKYRGB_LIGHT);
    DrawFillRect(fb, bx, by + bs - 1, bs, 1, SKYRGB_SHADOW);
    DrawFillRect(fb, bx + bs - 1, by, 1, bs, SKYRGB_SHADOW);
    DrawLine(fb, (int64_t)bx + 6, (int64_t)by + 6,
                 (int64_t)bx + bs - 7, (int64_t)by + bs - 7, SKYRGB_DARK);
    DrawLine(fb, (int64_t)bx + bs - 7, (int64_t)by + 6,
                 (int64_t)bx + 6, (int64_t)by + bs - 7, SKYRGB_DARK);

    // black inner text area; the flanterm grid lives inside it
    DrawFillRect(fb, SKYWIN_CONTENT_X, SKYWIN_CONTENT_Y,
                 SKYWIN_CONTENT_W, SKYWIN_CONTENT_H, SKYRGB_PAPER);
}

int main() {
    volatile uint64_t *proto = (volatile uint64_t*)SKYWIN_PROTO_PAGE_VA;

    FrameBuffer whole;
    whole.BaseAddress       = (void*)proto[SKYWIN_PROTO_WHOLE_VA];
    whole.BufferSize        = SKYWIN_W * SKYWIN_H * sizeof(uint32_t);
    whole.Width             = proto[SKYWIN_PROTO_WIN_W];
    whole.Height            = proto[SKYWIN_PROTO_WIN_H];
    whole.PixelsPerScanLine = proto[SKYWIN_PROTO_WIN_W];   // tightly packed

    if (!whole.BaseAddress || !whole.Width || !whole.Height) {
        syscall(SYSCALL_EXIT, 0, 0, 0, 0, 0, 0);  // protocol not published
        return 1;
    }

    // Reuse the libc console font for the title (loads it once, shared).
    TTF_Font *font = console_font();

    // Chrome first; the console grid only ever repaints the inner area.
    paint_window(&whole, font);

    // The requested console output (libc builds its context on first printf).
    printf("Hello World\n");
    printf("Skyline userspace console\n");
    printf("shared framebuffer OK\n");

    // First frame is fully painted: publish the ready handshake (release) so
    // the desktop only mounts/composites the surface once it is complete.
    // All surface stores above happen-before this release.
    __atomic_store_n((uint64_t*)&proto[SKYWIN_PROTO_READY],
                     (uint64_t)SKYWIN_READY_MAGIC, __ATOMIC_RELEASE);

    for (;;) sys_yield();   // stay resident; desktop recomposites the surface
    return 0;
}
