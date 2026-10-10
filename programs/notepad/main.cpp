//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// notepad: SkylineSystem's first interactive GUI app. It reads the keyboard
// event ring (mmap shared page, fed by the PS/2 and USB HID drivers through
// the kernel input core), edits a simple line-based text buffer, and draws
// text + caret into the WM-provided content surface.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/fb.h>
#include <graphic/winstyle.h>
#include <graphic/kbdshare.h>
#include <base/font/ttf/ttf.h>

#define NOTE_MAX_LINES 256
#define NOTE_MAX_COLS  200

static char g_lines[NOTE_MAX_LINES][NOTE_MAX_COLS];
static int  g_nLines = 1;
static int  g_curLine = 0;
static int  g_curCol  = 0;

/* ---- tiny line-buffer editor ------------------------------------------- */
static void ed_insert(char c) {
    int len = (int)strlen(g_lines[g_curLine]);
    if (len >= NOTE_MAX_COLS - 1) return;
    for (int i = len; i >= g_curCol; i--) g_lines[g_curLine][i + 1] = g_lines[g_curLine][i];
    g_lines[g_curLine][g_curCol] = c;
    g_curCol++;
}

static void ed_enter() {
    if (g_nLines >= NOTE_MAX_LINES - 1) return;
    for (int i = g_nLines; i > g_curLine; i--)
        strcpy(g_lines[i + 1], g_lines[i]);
    strcpy(g_lines[g_curLine + 1], &g_lines[g_curLine][g_curCol]);
    g_lines[g_curLine][g_curCol] = '\0';
    g_nLines++;
    g_curLine++;
    g_curCol = 0;
}

static void ed_backspace() {
    if (g_curCol > 0) {
        int len = (int)strlen(g_lines[g_curLine]);
        for (int i = g_curCol - 1; i < len; i++)
            g_lines[g_curLine][i] = g_lines[g_curLine][i + 1];
        g_curCol--;
    } else if (g_curLine > 0) {
        int prevLen = (int)strlen(g_lines[g_curLine - 1]);
        int curLen  = (int)strlen(g_lines[g_curLine]);
        if (prevLen + curLen >= NOTE_MAX_COLS) return;
        strcpy(&g_lines[g_curLine - 1][prevLen], g_lines[g_curLine]);
        for (int i = g_curLine; i < g_nLines - 1; i++)
            strcpy(g_lines[i], g_lines[i + 1]);
        g_lines[g_nLines - 1][0] = '\0';
        g_nLines--;
        g_curLine--;
        g_curCol = prevLen;
    }
}

static void ed_delete() {
    int len = (int)strlen(g_lines[g_curLine]);
    if (g_curCol < len) {
        for (int i = g_curCol; i < len; i++)
            g_lines[g_curLine][i] = g_lines[g_curLine][i + 1];
    } else if (g_curLine < g_nLines - 1) {
        int nxtLen = (int)strlen(g_lines[g_curLine + 1]);
        if (len + nxtLen < NOTE_MAX_COLS) {
            strcpy(&g_lines[g_curLine][len], g_lines[g_curLine + 1]);
            for (int i = g_curLine + 1; i < g_nLines - 1; i++)
                strcpy(g_lines[i], g_lines[i + 1]);
            g_lines[g_nLines - 1][0] = '\0';
            g_nLines--;
        }
    }
}

static void ed_left() {
    if (g_curCol > 0) g_curCol--;
    else if (g_curLine > 0) {
        g_curLine--;
        g_curCol = (int)strlen(g_lines[g_curLine]);
    }
}
static void ed_right() {
    int len = (int)strlen(g_lines[g_curLine]);
    if (g_curCol < len) g_curCol++;
    else if (g_curLine < g_nLines - 1) { g_curLine++; g_curCol = 0; }
}
static void ed_up() {
    if (g_curLine > 0) {
        g_curLine--;
        int len = (int)strlen(g_lines[g_curLine]);
        if (g_curCol > len) g_curCol = len;
    }
}
static void ed_down() {
    if (g_curLine < g_nLines - 1) {
        g_curLine++;
        int len = (int)strlen(g_lines[g_curLine]);
        if (g_curCol > len) g_curCol = len;
    }
}

static void ed_key(uint16_t key) {
    if (key >= 0x20u && key < 0x7Fu) { ed_insert((char)key); return; }
    switch (key) {
    case '\n': ed_enter(); return;
    case 0x08: ed_backspace(); return;
    case '\t': ed_insert('\t'); return;
    case KBD_KEY_LEFT:  ed_left(); return;
    case KBD_KEY_RIGHT: ed_right(); return;
    case KBD_KEY_UP:    ed_up(); return;
    case KBD_KEY_DOWN:  ed_down(); return;
    case KBD_KEY_HOME:  g_curCol = 0; return;
    case KBD_KEY_END:   g_curCol = (int)strlen(g_lines[g_curLine]); return;
    case KBD_KEY_DEL:   ed_delete(); return;
    default: return;
    }
}

int main() {
    /* The WM filled the fixed protocol page before launching us: it carries
       the content-area VA and geometry (read once, no handshake). */
    volatile uint64_t* q = (volatile uint64_t*)SKYWIN_PROTO_PAGE_VA;
    uint64_t contentVA = q[SKYWIN_PROTO_CONTENT_VA];
    uint32_t pitch    = (uint32_t)q[SKYWIN_PROTO_PITCH];
    uint32_t contentW = (uint32_t)q[SKYWIN_PROTO_CONTENT_W];
    uint32_t contentH = (uint32_t)q[SKYWIN_PROTO_CONTENT_H];
    if (!contentVA) return 1;

    /* Map the shared keyboard event ring. */
    uint64_t kbdVA = syscall(SYSCALL_DEV_MMAP, (uint64_t)KBD_DEV_TYPE,
                             0, 0, 0, 0, 0);
    if ((int64_t)kbdVA <= 0) return 1;
    KbdShared* kbd = (KbdShared*)kbdVA;
    int kbdSlot = kbd_reader_register(kbd, (int32_t)sys_getpid());
    uint64_t cursor = kbdSlot >= 0 ? kbd_reader_pos(kbd, kbdSlot) : 0;

    TTF_Font* font = nullptr;
    /* on-demand: read only the tables the rasterizer needs */
    TTF_ReadFontGlyfLazy(&font, "/mp/SourceHanSerifTC_Medium.ttf", 20, 256);
    if (!font) return 1;
    int32_t lineH = TTF_GetLineHeight(font);
    if (lineH <= 0) lineH = 22;

    FrameBuffer cb;
    cb.BaseAddress       = (void*)contentVA;
    cb.BufferSize        = q[SKYWIN_PROTO_CONTENT_SZ];
    /* PixelsPerScanLine is the wider shadow-bearing surface stride, while
       Width is the body-wide run so every fill/clip stops at the content edge
       and never paints the shadow margin (which squared off the right edge). */
    cb.PixelsPerScanLine = pitch;
    cb.Width             = contentW;
    cb.Height            = contentH;

    const int PAD_X = 8, PAD_Y = 6;
    const uint32_t PAPER = 0xFF0F0F12u;
    const uint32_t INK   = 0xFFE9E9EFu;
    const uint32_t CARET = 0xFF8FD3FFu;

    auto render = [&]() {
        DrawFillRect(&cb, 0, 0, contentW, contentH, PAPER);
        for (int i = 0; i < g_nLines; i++) {
            int y = PAD_Y + i * lineH;
            if (y + lineH > (int)contentH) break;
            if (g_lines[i][0])
                TTF_DrawText(&cb, font, PAD_X, y, g_lines[i], INK);
        }
        /* caret: a 2 px bar at the pixel width of the prefix text */
        char prefix[NOTE_MAX_COLS];
        memcpy(prefix, g_lines[g_curLine], (size_t)g_curCol);
        prefix[g_curCol] = '\0';
        int32_t px = 0, ph = 0;
        TTF_GetTextSize(font, prefix, &px, &ph);
        if (ph <= 0) ph = lineH;
        int cx = PAD_X + px;
        int cy = PAD_Y + g_curLine * lineH;
        DrawFillRect(&cb, cx, cy, 2, ph, CARET);
    };

    g_lines[0][0] = '\0';
    render();
    /* Universal readiness barrier: tell the WM our initial surface is
       painted. Notepad does not use the libc console, so OUT_SEQ stays 0 and
       READY alone lets the WM mount immediately. */
    q[SKYWIN_PROTO_CLIENT_READY] = SKYWIN_READY_MAGIC;

    for (;;) {
        uint64_t h = __atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE);
        cursor = kbd_reader_resync(kbd, cursor);
        bool changed = false;
        /* Focus gate: only the focused notepad edits text; a non-focused
           instance still drains the ring below. */
        bool allowed = (kbdSlot < 0) || kbd_focus_allows(kbd, kbdSlot);
        while (cursor < h) {
            KbdEvent e = kbd->ring[cursor & (KBD_RING_CAP - 1u)];
            if (allowed && e.action == KBD_ACTION_DOWN) { ed_key(e.key); changed = true; }
            cursor++;
        }
        if (kbdSlot >= 0) kbd_reader_setpos(kbd, kbdSlot, cursor);
        if (changed) render();
        sys_yield();   /* event loop: stay cheap, text editing is not 60 Hz */
    }
    return 0;
}
