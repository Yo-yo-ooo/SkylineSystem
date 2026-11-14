//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// notepad: SkylineSystem's first interactive GUI app. It reads the keyboard
// event ring (mmap shared page, fed by the PS/2 and USB HID drivers through
// the kernel input core), edits a simple line-based text buffer, and draws
// text + caret into the WM-provided content surface.
//
// File management (added with the Open/Close dialogs):
//   Ctrl+O  Open dialog  - file list from /mp/files.index (fallback built in)
//   Ctrl+W  Close dialog - Save and Close / Discard and Close / Cancel
//   Ctrl+S  Save the buffer to the current path (default /mp/notes.txt)
// A dialog is modal: while it is open, text editing keys are ignored and
// Up/Down/Enter/Esc drive the dialog. Everything is drawn with the same
// TTF + DrawFillRect primitives as the editor itself; the modal is centered
// from the *content* geometry, so it stays inside the surface on resize.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>            // INT32_MAX (脏行带哨兵)
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/fb.h>
#include <graphic/winstyle.h>
#include <graphic/kbdshare.h>
#include <base/font/ttf/ttf.h>
#include <base/prof.h>          // sky_rdtsc / SkyLat (输入延迟量化)

#define NOTE_MAX_LINES  256
#define NOTE_MAX_COLS   200
#define NOTE_MAX_PATH   192
#define NOTE_MAX_INDEX  16
#define NOTE_DEFAULT_PATH "/mp/notes.txt"
#define NOTE_INDEX_PATH   "/mp/files.index"
#define NOTE_STATUS_MS  2500u

/* A/B 量化开关 (P0-2): 1 = 优化前的行为 (每次变更整页重绘), 0 = 按行带局部
   重绘。同一份源码编出两个版本、跑同一个探针 (res/scripts/test/
   input_latency_probe.py), 才能给出可信的 before/after 数据。 */
#ifndef SKY_FULL_REDRAW
#define SKY_FULL_REDRAW 0
#endif

/* ------------------------------------------------------------------ */
/* editor state                                                        */
/* ------------------------------------------------------------------ */
static char g_lines[NOTE_MAX_LINES][NOTE_MAX_COLS];
static int  g_nLines = 1;
static int  g_curLine = 0;
static int  g_curCol  = 0;

/* current file path ("" = not associated yet -> default path on save) */
static char g_path[NOTE_MAX_PATH];

/* transient status line (Saved/Opened/errors) */
static char     g_status[NOTE_MAX_PATH + 64];
static bool     g_statusErr = false;
static uint64_t g_statusUntilMs = 0;

/* ------------------------------------------------------------------ */
/* dialog state                                                        */
/* ------------------------------------------------------------------ */
enum NoteDialog {
    DLG_NONE = 0,
    DLG_OPEN,
    DLG_CLOSE,
};

struct NoteIndexEntry {
    char label[56];
    char path[NOTE_MAX_PATH];
};

static NoteIndexEntry g_index[NOTE_MAX_INDEX];
static int  g_nIndex = 0;
static int  g_dialog = DLG_NONE;
static int  g_dlgSel = 0;
static int  g_dlgScroll = 0;
static char g_dlgErr[128];

static const char* const kCloseItems[3] = {
    "Save and Close",
    "Discard and Close",
    "Cancel",
};

/* 字体句柄 (main 里创建): 让 note_* 辅助函数也能打 TTF 缓存统计。 */
static TTF_Font* g_font = nullptr;
/* 延迟诊断的窗口级计数 (每 32 个按键批次清零一次) */
static uint32_t g_qMax = 0;      /* 本窗口内见过的最大"环里积压事件数" */
static uint32_t g_e2eSkip = 0;   /* 端到端样本里被丢弃的跨核异常值 */
/* 三个直方图放到文件作用域: note_quick_save() 也要能把窗口清零 (探针用
   Ctrl+S 把"铺文档"阶段的冷启动样本丢掉, 再测稳态输入)。 */
static SkyLat g_lat;             /* 取出 -> 画完 (同线程 TSC) */
static SkyLat g_latRender;       /* 纯绘制 */
static SkyLat g_latE2E;          /* 内核入环 -> 画完 */

static void note_log(const char* fmt, ...);   /* 定义在后面, 这里先声明 */

/* 文档字符数 (含 '\n'): 门禁用它证明"注入 N 个字符, 收到的就是 N 个",
   即没有丢字/乱序。 */
static uint64_t note_total_chars(void) {
    uint64_t n = 0;
    for (int i = 0; i < g_nLines; i++) n += (uint64_t)strlen(g_lines[i]);
    if (g_nLines > 1) n += (uint64_t)(g_nLines - 1);   /* 行间换行符 */
    return n;
}

/* TTF 缓存命中率 + lazy glyf 的读盘统计 (证明"热路径不再碰字形文件")。 */
static void note_log_ttf(const char* tag) {
    if (!g_font) return;
    TTF_CacheStats cs;
    TTF_GetCacheStats(g_font, &cs);
    TTF_AtlasStats as;
    TTF_GetAtlasStats(g_font, &as);
    unsigned long cap = 0, resident = 0, fetches = 0, bytes = 0;
    TTF_GetGlyfStats(g_font, &cap, &resident, &fetches, &bytes);
    unsigned long tot = cs.hit_count + cs.miss_count;
    unsigned long pct = tot ? (cs.hit_count * 100u) / tot : 0u;
    note_log("[notepad] ttf %s: hit=%lu miss=%lu hit%%=%lu atlas glyphs=%d/%d "
             "evict=%lu | glyf win=%luKB res=%lu fetch=%lu bytes=%lu\n",
             tag, cs.hit_count, cs.miss_count, pct,
             (int)as.glyph_count, (int)as.glyph_max, as.evict_count,
             cap / 1024u, resident, fetches, bytes);
}

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

/* ================================================================== */
/* small helpers                                                       */
/* ================================================================== */
static uint64_t note_now_ms(void) {
    return (uint64_t)syscall(SYSCALL_MONOTONIC_MS, 0, 0, 0, 0, 0, 0);
}

static bool note_ctrl_held(uint8_t mods) {
    return (mods & (uint8_t)(KBD_MOD_LCTRL | KBD_MOD_RCTRL)) != 0;
}

static bool note_alt_held(uint8_t mods) {
    return (mods & (uint8_t)(KBD_MOD_LALT | KBD_MOD_RALT)) != 0;
}

static void note_copy(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1 < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

static void note_set_path(const char* path) {
    note_copy(g_path, sizeof(g_path), path);
}

static const char* note_current_path(void) {
    return g_path[0] ? g_path : NOTE_DEFAULT_PATH;
}

static void note_set_status(const char* text, bool is_error) {
    note_copy(g_status, sizeof(g_status), text);
    g_statusErr = is_error;
    g_statusUntilMs = note_now_ms() + NOTE_STATUS_MS;
}

/* ================================================================== */
/* serial markers for the QEMU headless gate (SYSCALL_DBGSOUT = 24)    */
/* ------------------------------------------------------------------ */
/* One marker per real event, never per frame. Every line ends with    */
/* '\n'; the formats are part of the automation contract, keep them    */
/* stable ("[notepad] dialog=open items=<N>" etc.).                    */
/* ================================================================== */
static void note_serial(const char* text, size_t len) {
    if (!text || len == 0) return;
    syscall(SYS_DBGSOUT, (uint64_t)text, (uint64_t)len, 0, 0, 0, 0);
}

static void note_log(const char* fmt, ...) {
    char buf[320];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    note_serial(buf, (size_t)n);
}

/* ================================================================== */
/* file I/O (stdio from libsky)                                        */
/* ================================================================== */
/* Save the whole buffer (lines joined by '\n') to `path`.
 * Returns 0 on success; on failure writes a short reason to err.
 * bytes_out (optional) receives the number of bytes actually written. */
static int note_save(const char* path, char* err, size_t errcap,
                     uint64_t* bytes_out) {
    if (bytes_out) *bytes_out = 0;
    FILE* f = fopen(path, "w");
    if (!f) {
        snprintf(err, errcap, "cannot open for write");
        return -1;
    }
    uint64_t written = 0;
    for (int i = 0; i < g_nLines; i++) {
        size_t len = strlen(g_lines[i]);
        if (len && fwrite(g_lines[i], 1, len, f) != len) {
            snprintf(err, errcap, "write failed");
            fclose(f);
            return -1;
        }
        written += (uint64_t)len;
        if (i + 1 < g_nLines) {
            if (fwrite("\n", 1, 1, f) != 1) {
                snprintf(err, errcap, "write failed");
                fclose(f);
                return -1;
            }
            written += 1u;
        }
    }
    if (fclose(f) != 0) {
        snprintf(err, errcap, "flush/close failed");
        return -1;
    }
    if (bytes_out) *bytes_out = written;
    return 0;
}

/* Load a text file into g_lines: split on '\n', drop CR (CRLF files),
 * truncate lines longer than NOTE_MAX_COLS-1 and cap at NOTE_MAX_LINES.
 * bytes_out (optional) receives the number of file bytes consumed. */
static int note_load(const char* path, char* err, size_t errcap,
                     bool* truncated_out, uint64_t* bytes_out) {
    if (truncated_out) *truncated_out = false;
    if (bytes_out) *bytes_out = 0;
    FILE* f = fopen(path, "r");
    if (!f) {
        snprintf(err, errcap, "file not found");
        return -1;
    }

    int line = 0;
    int col = 0;
    bool truncated = false;
    int nlines = 1;
    int c;
    uint64_t bytes = 0;
    g_lines[0][0] = '\0';
    while ((c = fgetc(f)) != EOF) {
        bytes++;
        if (c == '\r') continue;              /* tolerate CRLF */
        if (c == '\n') {
            g_lines[line][col] = '\0';
            if (line + 1 >= NOTE_MAX_LINES) { /* too many lines: stop */
                truncated = true;
                break;
            }
            line++;
            col = 0;
            g_lines[line][0] = '\0';
            nlines = line + 1;
        } else {
            if (col < NOTE_MAX_COLS - 1)
                g_lines[line][col++] = (char)c;
            else
                truncated = true;             /* drop rest of this line */
        }
    }
    if (ferror(f)) {
        fclose(f);
        snprintf(err, errcap, "read error");
        return -1;
    }
    fclose(f);

    g_lines[line][col] = '\0';
    if (nlines < line + 1) nlines = line + 1;
    g_nLines = nlines;
    g_curLine = 0;
    g_curCol = 0;
    if (truncated_out) *truncated_out = truncated;
    if (bytes_out) *bytes_out = bytes;
    return 0;
}

/* Trim trailing CR/LF/spaces/tabs in place. */
static void note_trim_right(char* s) {
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') s[--n] = '\0';
        else break;
    }
}

/* Read /mp/files.index: one "label|/mp/path" per line. If the file is
 * missing/empty/malformed we fall back to a small built-in list so the
 * Open dialog is always usable (Installer normally writes the index). */
static void note_load_index(void) {
    g_nIndex = 0;
    FILE* f = fopen(NOTE_INDEX_PATH, "r");
    if (f) {
        char raw[256];
        while (g_nIndex < NOTE_MAX_INDEX && fgets(raw, (int)sizeof(raw), f)) {
            note_trim_right(raw);
            if (!raw[0]) continue;
            char* bar = strchr(raw, '|');
            if (!bar) continue;
            *bar = '\0';
            char* label = raw;
            char* path = bar + 1;
            while (*path == ' ' || *path == '\t') path++;
            if (!path[0]) continue;
            /* trim label trailing spaces */
            char* le = bar;
            while (le > label && (le[-1] == ' ' || le[-1] == '\t')) *--le = '\0';
            if (!label[0]) label = path;
            note_copy(g_index[g_nIndex].label, sizeof(g_index[0].label), label);
            note_copy(g_index[g_nIndex].path, sizeof(g_index[0].path), path);
            g_nIndex++;
        }
        fclose(f);
    }
    if (g_nIndex == 0) {
        static const char* const kFallback[3][2] = {
            {"notes.txt", NOTE_DEFAULT_PATH},
            {"desktop.lua", "/mp/desktop.lua"},
            {"theme_aurora.lua", "/mp/theme_aurora.lua"},
        };
        for (int i = 0; i < 3 && g_nIndex < NOTE_MAX_INDEX; i++) {
            note_copy(g_index[g_nIndex].label, sizeof(g_index[0].label),
                      kFallback[i][0]);
            note_copy(g_index[g_nIndex].path, sizeof(g_index[0].path),
                      kFallback[i][1]);
            g_nIndex++;
        }
    }
}

/* ================================================================== */
/* dialogs                                                             */
/* ================================================================== */
static void note_statusf(bool is_error, const char* fmt, const char* a,
                         const char* b) {
    char msg[NOTE_MAX_PATH + 64];
    snprintf(msg, sizeof(msg), fmt, a ? a : "", b ? b : "");
    note_set_status(msg, is_error);
}

static void note_quick_save(void) {
    const char* path = note_current_path();
    char err[96];
    uint64_t nbytes = 0;
    if (note_save(path, err, sizeof(err), &nbytes) == 0) {
        if (!g_path[0]) note_set_path(path);
        note_statusf(false, "Saved: %s", g_path, "");
        note_log("[notepad] saved path=%s bytes=%llu\n", g_path,
                 (unsigned long long)nbytes);
        /* 门禁用: 文档字符数必须等于注入的键数 (证明无丢字/乱序),
           顺便打一次 TTF 缓存命中率。 */
        note_log("[notepad] len=%llu lines=%d\n",
                 (unsigned long long)note_total_chars(), g_nLines);
        note_log_ttf("save");
        /* 保存 = 重新开始一个测量窗口: 探针先 Ctrl+S 把"铺文档"阶段
           (冷缓存/大量首见字形) 的样本丢掉, 再输入被测量的字符,
           这样读到的分位数是**稳态**输入, 不掺冷启动。 */
        sky_lat_reset(&g_lat);
        sky_lat_reset(&g_latRender);
        sky_lat_reset(&g_latE2E);
        g_qMax = 0;
        g_e2eSkip = 0;
    } else {
        note_statusf(true, "Save failed: %s (%s)", path, err);
        note_log("[notepad] save_failed path=%s\n", path);
    }
}

static int note_dlg_item_count(void) {
    if (g_dialog == DLG_OPEN) return g_nIndex + 1; /* files + Cancel */
    if (g_dialog == DLG_CLOSE) return 3;
    return 0;
}

static const char* note_dlg_item_label(int idx) {
    if (g_dialog == DLG_OPEN) {
        if (idx >= 0 && idx < g_nIndex) return g_index[idx].label;
        return "Cancel";
    }
    if (g_dialog == DLG_CLOSE && idx >= 0 && idx < 3) return kCloseItems[idx];
    return "";
}

static void note_dlg_begin(int which) {
    g_dlgErr[0] = '\0';
    g_dlgSel = 0;
    g_dlgScroll = 0;
    if (which == DLG_OPEN) {
        note_load_index();
        g_dialog = DLG_OPEN;
        note_log("[notepad] dialog=open items=%d\n", g_nIndex);
    } else {
        g_dialog = DLG_CLOSE;
        note_log("[notepad] dialog=close\n");
    }
}

static void note_dlg_begin_open(void) { note_dlg_begin(DLG_OPEN); }
static void note_dlg_begin_close(void) { note_dlg_begin(DLG_CLOSE); }

static void note_dlg_activate(int idx) {
    if (g_dialog == DLG_OPEN) {
        if (idx < 0 || idx >= g_nIndex) {        /* Cancel */
            g_dialog = DLG_NONE;
            g_dlgErr[0] = '\0';
            return;
        }
        char err[96];
        bool truncated = false;
        uint64_t nbytes = 0;
        if (note_load(g_index[idx].path, err, sizeof(err), &truncated,
                      &nbytes) != 0) {
            snprintf(g_dlgErr, sizeof(g_dlgErr), "Open failed: %s", err);
            note_log("[notepad] open_failed path=%s\n", g_index[idx].path);
            return;
        }
        note_set_path(g_index[idx].path);
        g_dialog = DLG_NONE;
        g_dlgErr[0] = '\0';
        note_log("[notepad] opened path=%s bytes=%llu\n", g_path,
                 (unsigned long long)nbytes);
        if (truncated)
            note_statusf(false, "Opened: %s (truncated)", g_path, "");
        else
            note_statusf(false, "Opened: %s", g_path, "");
        return;
    }

    if (g_dialog == DLG_CLOSE) {
        if (idx == 1) {                          /* Discard and Close */
            note_log("[notepad] close choice=discard\n");
            note_log("[notepad] len=%llu lines=%d\n",
                     (unsigned long long)note_total_chars(), g_nLines);
            note_log_ttf("exit");
            note_log("[notepad] exit saved=0\n");
            sys_exit(0);
            return;
        }
        if (idx != 0) {                          /* Cancel */
            note_log("[notepad] close choice=cancel\n");
            g_dialog = DLG_NONE;
            g_dlgErr[0] = '\0';
            return;
        }
        /* Save and Close */
        note_log("[notepad] close choice=save\n");
        const char* path = note_current_path();
        char err[96];
        uint64_t nbytes = 0;
        if (note_save(path, err, sizeof(err), &nbytes) != 0) {
            snprintf(g_dlgErr, sizeof(g_dlgErr), "Save failed: %s", err);
            note_log("[notepad] save_failed path=%s\n", path);
            return;
        }
        if (!g_path[0]) note_set_path(path);
        note_log("[notepad] saved path=%s bytes=%llu\n", g_path,
                 (unsigned long long)nbytes);
        note_log("[notepad] len=%llu lines=%d\n",
                 (unsigned long long)note_total_chars(), g_nLines);
        note_log_ttf("exit");
        note_log("[notepad] exit saved=1\n");
        sys_exit(0);                             /* WM reaps the window */
    }
}

static void note_dlg_key(uint16_t key) {
    int n = note_dlg_item_count();
    if (n <= 0) { g_dialog = DLG_NONE; return; }
    switch (key) {
    case KBD_KEY_UP:
        if (g_dlgSel > 0) g_dlgSel--;
        return;
    case KBD_KEY_DOWN:
        if (g_dlgSel < n - 1) g_dlgSel++;
        return;
    case '\n':
    case '\r':
        note_dlg_activate(g_dlgSel);
        return;
    case 0x1B:                                   /* Esc */
        if (g_dialog == DLG_CLOSE)
            note_log("[notepad] close choice=cancel\n");
        g_dialog = DLG_NONE;
        g_dlgErr[0] = '\0';
        return;
    default:
        return;
    }
}

/* ================================================================== */
/* rendering helpers                                                   */
/* ================================================================== */
/* Draw text clipped to maxW pixels (shrink until it fits). UTF-8 safe:
 * shrinking never leaves a dangling continuation byte. */
static void note_text_fit(FrameBuffer* fb, TTF_Font* font, int32_t x, int32_t y,
                          const char* text, uint32_t color, int32_t maxW) {
    if (!fb || !font || !text || !text[0] || maxW <= 0) return;
    char buf[NOTE_MAX_COLS];
    size_t len = strlen(text);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, text, len);
    buf[len] = '\0';
    while (len > 0) {
        int32_t w = 0, h = 0;
        TTF_GetTextSize(font, buf, &w, &h);
        if (w <= maxW) break;
        len--;
        while (len > 0 && ((unsigned char)buf[len] & 0xC0u) == 0x80u)
            len--;                               /* back over UTF-8 tail */
        buf[len] = '\0';
    }
    if (len == 0) return;
    TTF_DrawText(fb, font, x, y, buf, color);
}

static void note_draw_status(FrameBuffer* cb, TTF_Font* font, int32_t lineH,
                             uint32_t contentW, uint32_t contentH) {
    if (!g_status[0]) return;
    int32_t y = (int32_t)contentH - lineH - 1;
    if (y < 0) y = 0;
    DrawFillRect(cb, 0, (uint64_t)y, contentW, (uint64_t)(lineH + 1), 0xFF121216u);
    note_text_fit(cb, font, 8, y + 1, g_status,
                  g_statusErr ? 0xFFFF8A8Au : 0xFF8FD3A0u,
                  (int32_t)contentW - 16);
}

static void note_draw_dialog(FrameBuffer* cb, TTF_Font* font, int32_t lineH,
                             uint32_t contentWArg, uint32_t contentHArg) {
    if (g_dialog == DLG_NONE) return;
    int32_t W = (int32_t)contentWArg;
    int32_t H = (int32_t)contentHArg;
    if (W < 40 || H < 40) return;

    int32_t n = note_dlg_item_count();
    if (n <= 0) return;

    int32_t titleH  = lineH + 10;
    int32_t itemH   = lineH + 6;
    int32_t footerH = lineH + 8 + (g_dlgErr[0] ? (lineH + 2) : 0);

    int32_t mw = W - 24;
    if (mw > 460) mw = 460;
    if (mw < 120) mw = W;

    int32_t maxRows = (H - 12 - titleH - footerH) / itemH;
    if (maxRows < 1) maxRows = 1;
    int32_t rows = n < maxRows ? n : maxRows;
    int32_t mh = titleH + rows * itemH + footerH + 12;
    if (mh > H - 8) mh = H - 8;
    if (mh < titleH + footerH + itemH) return;   /* too small to draw safely */

    /* keep selection visible inside the scroll window */
    if (g_dlgSel < 0) g_dlgSel = 0;
    if (g_dlgSel >= n) g_dlgSel = n - 1;
    if (g_dlgSel < g_dlgScroll) g_dlgScroll = g_dlgSel;
    if (g_dlgSel >= g_dlgScroll + rows) g_dlgScroll = g_dlgSel - rows + 1;
    if (g_dlgScroll > n - rows) g_dlgScroll = n - rows;
    if (g_dlgScroll < 0) g_dlgScroll = 0;

    int32_t mx = (W - mw) / 2;
    int32_t my = (H - mh) / 2;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx + mw > W) mw = W - mx;
    if (my + mh > H) mh = H - my;

    /* body + border + title bar */
    DrawFillRect(cb, (uint64_t)mx, (uint64_t)my, (uint64_t)mw, (uint64_t)mh,
                 0xFF15151Au);
    DrawFillRect(cb, (uint64_t)mx, (uint64_t)my, (uint64_t)mw, 1, 0xFF4A4A55u);
    DrawFillRect(cb, (uint64_t)mx, (uint64_t)(my + mh - 1), (uint64_t)mw, 1,
                 0xFF4A4A55u);
    DrawFillRect(cb, (uint64_t)mx, (uint64_t)my, 1, (uint64_t)mh, 0xFF4A4A55u);
    DrawFillRect(cb, (uint64_t)(mx + mw - 1), (uint64_t)my, 1, (uint64_t)mh,
                 0xFF4A4A55u);
    DrawFillRect(cb, (uint64_t)(mx + 1), (uint64_t)(my + 1),
                 (uint64_t)(mw - 2), (uint64_t)titleH, 0xFF26262Au);
    DrawFillRect(cb, (uint64_t)(mx + 1), (uint64_t)(my + 1 + titleH),
                 (uint64_t)(mw - 2), 1, 0xFF3D3D44u);

    const char* title = (g_dialog == DLG_OPEN) ? "Open File" : "Close Document";
    note_text_fit(cb, font, mx + 10, my + 5, title, 0xFFECECECu, mw - 20);

    /* list */
    for (int32_t r = 0; r < rows; r++) {
        int32_t idx = g_dlgScroll + r;
        if (idx >= n) break;
        int32_t iy = my + 1 + titleH + 4 + r * itemH;
        if (iy + itemH > my + mh - footerH) break;
        if (idx == g_dlgSel) {
            DrawFillRect(cb, (uint64_t)(mx + 8), (uint64_t)iy,
                         (uint64_t)(mw - 16), (uint64_t)itemH, 0xFF34343Cu);
            DrawFillRect(cb, (uint64_t)(mx + 8), (uint64_t)iy, 3,
                         (uint64_t)itemH, 0xFF60CDFFu);
        }
        note_text_fit(cb, font, mx + 18, iy + 2, note_dlg_item_label((int)idx),
                      (idx == g_dlgSel) ? 0xFFFFFFFFu : 0xFFD0D0D8u, mw - 36);
    }

    /* scroll hint when the list does not fit */
    if (n > rows && mw >= 140) {
        char cnt[32];
        snprintf(cnt, sizeof(cnt), "%d-%d/%d", (int)(g_dlgScroll + 1),
                 (int)(g_dlgScroll + rows), (int)n);
        note_text_fit(cb, font, mx + mw - 8 - 72, my + 5, cnt, 0xFF9A9AA5u, 64);
    }

    /* error line (only one, keeps the modal inside the surface) */
    if (g_dlgErr[0]) {
        note_text_fit(cb, font, mx + 10, my + mh - footerH - lineH - 2,
                      g_dlgErr, 0xFFFF8A8Au, mw - 20);
    }

    /* footer hints */
    const char* hint = (g_dialog == DLG_OPEN)
        ? "Up/Down: select   Enter: open   Esc: cancel"
        : "Up/Down: select   Enter: confirm   Esc: cancel";
    note_text_fit(cb, font, mx + 10, my + mh - lineH - 5, hint, 0xFF9A9AA5u,
                  mw - 20);
}

/* ================================================================== */
/* key dispatch (editor vs modal)                                      */
/* ================================================================== */
static void note_handle_key(uint16_t key, uint8_t mods) {
    if (g_dialog != DLG_NONE) { note_dlg_key(key); return; }

    if (note_ctrl_held(mods)) {
        /* Ctrl+Alt+* 是 desktop 的全局快捷键（Settings/Notepad/Installer）；
         * 客户端必须忽略，否则 Notepad 会把 Ctrl+Alt+S 误当成 Ctrl+S 保存。 */
        if (note_alt_held(mods)) return;
        if (key == 'o' || key == 'O') { note_dlg_begin_open(); return; }
        if (key == 'w' || key == 'W') { note_dlg_begin_close(); return; }
        if (key == 's' || key == 'S') { note_quick_save(); return; }
        return;   /* consume other Ctrl combos so they never insert text */
    }
    ed_key(key);
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
    g_font = font;          /* 供 note_log_ttf() 打缓存统计 */
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

    /* ---- 局部重绘 ---------------------------------------------------------
     * 原来每次 changed 都整页重绘: contentW*contentH*4 B 的填充 (~850 KB)
     * 加上每一行一次 TTF_DrawText。QEMU TCG 下一帧 10~30 ms, 而事件循环
     * 每个按键都会走一次 —— 快速打字时延迟就按"键数 × 单帧"累积。
     * 现在按**像素行带**重绘: 只清掉真正变了的那几行再重画, 其余像素原样
     * 保留 (内容区是 WM 共享的 surface, 没人会擦掉它)。
     * 弹窗 (Open/Close) 是覆盖整块的模态层, 仍然整页重绘 —— 它不是打字热
     * 路径, 而且必须盖住下面的文本。整页路径的帧提交由 render_full() 推迟
     * 到模态层也写完之后统一执行, 不在 render_band 里提前递增 OUT_SEQ。 */
    /* WM 只靠 OUT_SEQ 变化才把客户端 surface 搬运/重合成; release 语义保证
       序号对 WM 可见之前, 本次帧的所有像素写入已经完成。因此提交必须是每条
       绘制路径的最后一步, 并集中在一处, 避免模态层被漏在提交之后。 */
    /* 本帧累积的脏行带 (内容坐标, 半开区间)。
       每个 render_band 把自己的范围并进来, commit_frame 一口气发布给 WM。
       打字时这通常只有一行高 (17 px), WM 因此只重合成那 17 行, 而不是整屏
       800 行 —— 这是"按键到上屏"延迟从几十毫秒降到个位数的主路径。
       哨兵用 INT32_MAX / -1: 空带 (y1 <= y0) 表示"本帧一个像素都没动",
       此时不发布脏带, 也不递增 dirty_seq。 */
    int32_t frameY0 = INT32_MAX, frameY1 = -1;

    auto commit_frame = [&]() {
        /* 顺序: 带 -> dirty_seq -> OUT_SEQ。全部 release, WM 看到 OUT_SEQ
           变化时, 像素与脏带都已在共享页里可见。 */
        if (frameY1 > frameY0) {
            q[SKYWIN_PROTO_DIRTY_Y0] = (uint64_t)frameY0;
            q[SKYWIN_PROTO_DIRTY_Y1] = (uint64_t)frameY1;
            __atomic_add_fetch(&q[SKYWIN_PROTO_DIRTY_SEQ], 1u, __ATOMIC_RELEASE);
        }
        frameY0 = INT32_MAX;
        frameY1 = -1;
        __atomic_add_fetch(&q[SKYWIN_PROTO_OUT_SEQ], 1u, __ATOMIC_RELEASE);
    };
    auto render_band = [&](int32_t y0, int32_t y1, bool commit) {
        if (y0 < 0) y0 = 0;
        if (y1 > (int32_t)contentH) y1 = (int32_t)contentH;
        if (y0 >= y1) return;
        if (y0 < frameY0) frameY0 = y0;
        if (y1 > frameY1) frameY1 = y1;
        DrawFillRect(&cb, 0, (uint64_t)y0, contentW, (uint64_t)(y1 - y0), PAPER);
        for (int i = 0; i < g_nLines; i++) {
            int y = PAD_Y + i * lineH;
            if (y + lineH <= y0) continue;
            if (y >= y1) break;
            if (g_lines[i][0])
                TTF_DrawText(&cb, font, PAD_X, y, g_lines[i], INK);
        }
        if (g_dialog == DLG_NONE) {
            /* caret: a 2 px bar at the pixel width of the prefix text.
               只在它所在的行带被重绘时补画 (旧光标已被同一带的清屏擦掉)。 */
            char prefix[NOTE_MAX_COLS];
            int n = g_curCol;
            if (n < 0) n = 0;
            if (n > NOTE_MAX_COLS - 1) n = NOTE_MAX_COLS - 1;
            memcpy(prefix, g_lines[g_curLine], (size_t)n);
            prefix[n] = '\0';
            int32_t px = 0, ph = 0;
            TTF_GetTextSize(font, prefix, &px, &ph);
            if (ph <= 0) ph = lineH;
            int cx = PAD_X + px;
            int cy = PAD_Y + g_curLine * lineH;
            if (cy < y1 && cy + lineH > y0)
                DrawFillRect(&cb, cx, cy, 2, ph, CARET);
        }
        /* 状态栏自带底色填充, 只在行带相交时画 (它会盖住底部的空行) */
        const int32_t sy = (int32_t)contentH - lineH - 1;
        if (sy < y1 && sy + lineH + 1 > y0)
            note_draw_status(&cb, font, lineH, contentW, contentH);

        /* 帧提交: 曾经这里从不递增, 打完字只能等 2Hz 任务栏时钟 (500ms)
           兜底, 实测按键->上屏 p50 414ms / p99 448ms。现在提交必须发生在
           **全部**像素写入之后 (含模态层): 局部重绘 (打字) 以 commit=true
           在这里就地提交; render_full() 以 commit=false 绘制底图, 把提交
           推迟到 note_draw_dialog() 画完模态层之后, 再调用 commit_frame()。
           release 语义保证这些像素写入先于 OUT_SEQ 对 WM 可见。 */
        if (commit) commit_frame();
    };
    auto render_full = [&]() {
        render_band(0, (int32_t)contentH, false);
        note_draw_dialog(&cb, font, lineH, contentW, contentH);
        commit_frame();   /* 帧提交必须在所有像素（含模态层）写完之后 */
    };

    /* 输入延迟样本 (每 32 个按键批次汇总一次 p50/p99 到串口) */
    sky_lat_reset(&g_lat);
    sky_lat_reset(&g_latRender);
    sky_lat_reset(&g_latE2E);
    g_qMax = 0; g_e2eSkip = 0;
    (void)sky_tsc_per_ms();       /* 预热 TSC 标定, 别算进第一个样本 */

    /* 预热常用 ASCII 字形: 字形缓存是进程内的, 新起的 Notepad 缓存是冷的,
       第一个按键才惰性加载 glyf —— QEMU TCG 下实测首键上屏 568 ms, 而
       稳态只有 115 ms。这里在 READY 之前逐字栅格化一遍(都画在 (4,4),
       单字符不会越界), 随后 render_full() 清屏覆盖, 把冷启动成本挪到
       用户开始打字之前。 */
    static const char kWarmGlyphs[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
        " .,;:/-_()[]{}'\"=+*#@!?<>";
    for (const char* p = kWarmGlyphs; *p; ++p) {
        char one[2] = {*p, '\0'};
        TTF_DrawText(&cb, font, 4, 4, one, INK);
    }

    g_lines[0][0] = '\0';
    render_full();
    /* Universal readiness barrier: tell the WM our initial surface is
       painted. READY 是挂载信号; 输出帧的搬运另由 OUT_SEQ 递增驱动
       (见 commit_frame / render_band 末尾), 两者互不影响。 */
    q[SKYWIN_PROTO_CLIENT_READY] = SKYWIN_READY_MAGIC;
    /* Serial marker for the QEMU headless gate: first frame painted + READY. */
    note_log("[notepad] start content=%ux%u\n", (unsigned)contentW,
             (unsigned)contentH);

    for (;;) {
        uint64_t h = __atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE);
        cursor = kbd_reader_resync(kbd, cursor);
        bool keyChanged = false;
        bool statusChanged = false;
        uint64_t tFirstKey = 0;   /* 本批第一个按键被取出的时刻 (TSC) */
        /* 本批开始时环里已经积压了多少事件。这是"输入是否在等渲染"的
           **无时钟偏差**证据: 处理及时时它应当恒为 0~1; 一旦客户端被一帧
           拖住, 按键就会在环里排队, 这个值线性增长。 */
        const uint64_t qPending = (h > cursor) ? (h - cursor) : 0u;
        if (qPending > (uint64_t)g_qMax) g_qMax = (uint32_t)qPending;
        /* 内核入环时刻 (KbdEvent.tsc_lo, 低 32 位): 端到端延迟起点。 */
        uint32_t tKernel = 0;
        /* 变化前的光标/行数/弹窗态: 决定局部重绘的行带 */
        const int prevCurLine = g_curLine;
        const int prevNLines  = g_nLines;
        const int prevDialog  = g_dialog;
        const bool hadStatus  = (g_status[0] != 0);
        /* Focus gate: only the focused notepad edits text; a non-focused
           instance still drains the ring below. */
        bool allowed = (kbdSlot < 0) || kbd_focus_allows(kbd, kbdSlot);
        while (cursor < h) {
            KbdEvent e = kbd->ring[cursor & (KBD_RING_CAP - 1u)];
            if (allowed && e.action == KBD_ACTION_DOWN) {
                if (tFirstKey == 0) {
                    tFirstKey = sky_rdtsc();
                    /* 平行数组里的入环时刻 (与事件同槽位) */
                    tKernel = kbd->stamp[cursor & (KBD_RING_CAP - 1u)];
                }
                note_handle_key(e.key, e.mods);
                keyChanged = true;
            }
            cursor++;
        }
        if (kbdSlot >= 0) kbd_reader_setpos(kbd, kbdSlot, cursor);
        /* Expire the transient status line (Saved:/Opened:/error). */
        if (g_status[0] && note_now_ms() >= g_statusUntilMs) {
            g_status[0] = '\0';
            statusChanged = true;
        }
        if (keyChanged || statusChanged) {
            const bool statusVisible = (g_status[0] != 0) || hadStatus;
            const uint64_t t0 = keyChanged ? tFirstKey : sky_rdtsc();
            const uint64_t tr0 = sky_rdtsc();      /* 纯绘制耗时起点 */
            if (SKY_FULL_REDRAW) {
                render_full();               /* A/B: 优化前的整页重绘 */
            } else if (g_dialog != DLG_NONE || prevDialog != DLG_NONE) {
                render_full();               /* 模态层: 必须整页重绘 */
            } else if (g_nLines != prevNLines) {
                /* 行数变了 (Enter / 行首退格 / Delete 合并行): 从最早变化的
                   那一行一直重画到文本区末尾 —— 下面的行整体位移了。 */
                int from = prevCurLine < g_curLine ? prevCurLine : g_curLine;
                if (from < 0) from = 0;
                int32_t y0 = PAD_Y + from * lineH;
                int32_t y1 = (int32_t)contentH;   /* 一直重画到文本区末尾 */
                render_band(y0, y1, true);
            } else {
                /* 行数不变: 只需重画"旧光标行 ∪ 新光标行" (插入/退格只改
                   当前行; 上下键只是光标换行)。 */
                int lo = prevCurLine < g_curLine ? prevCurLine : g_curLine;
                int hi = prevCurLine > g_curLine ? prevCurLine : g_curLine;
                int32_t y0 = PAD_Y + lo * lineH;
                int32_t y1 = PAD_Y + (hi + 1) * lineH;
                /* 状态条可见时把它一并纳入损伤区 (它是覆盖层, 不随文本行清屏
                   而消失, 但文本变化可能改变它的可见性)。 */
                if (statusVisible) y1 = (int32_t)contentH;
                render_band(y0, y1, true);
            }
            const uint64_t nowTsc = sky_rdtsc();
            const uint64_t renderUs = sky_tsc_to_us(nowTsc - tr0);
            if (keyChanged) {
                /* (a) 取出 -> 画完 (同线程 TSC, 无跨核偏差, 与历史数据可比) */
                if (sky_lat_add(&g_lat, sky_tsc_to_us(nowTsc - t0))) {
                    uint32_t p50 = 0, p99 = 0;
                    int n = 0;
                    sky_lat_report(&g_lat, &p50, &p99, &n);
                    note_log("[notepad] input_lat n=%d p50=%uus p99=%uus "
                             "qmax=%u e2e_skip=%u\n",
                             n, (unsigned)p50, (unsigned)p99, (unsigned)g_qMax,
                             (unsigned)g_e2eSkip);
                    g_qMax = 0;
                }
                /* (b) 内核入环 -> 画完 (端到端)。TSC 在 SMP 下可能跨核偏移,
                       离谱值 (>=200ms) 只计数不采样, 避免污染分位数。 */
                if (tKernel) {
                    const uint32_t dcyc = (uint32_t)((uint32_t)nowTsc - tKernel);
                    const uint64_t e2eUs = sky_tsc_to_us(dcyc);
                    if (e2eUs < 200000ull) {
                        if (sky_lat_add(&g_latE2E, e2eUs)) {
                            uint32_t e50 = 0, e99 = 0;
                            int en = 0;
                            sky_lat_report(&g_latE2E, &e50, &e99, &en);
                            note_log("[notepad] e2e_lat n=%d p50=%uus p99=%uus "
                                     "skip=%u\n",
                                     en, (unsigned)e50, (unsigned)e99,
                                     (unsigned)g_e2eSkip);
                        }
                    } else {
                        g_e2eSkip++;
                    }
                }
                if (sky_lat_add(&g_latRender, renderUs)) {
                    uint32_t r50 = 0, r99 = 0;
                    int rn = 0;
                    sky_lat_report(&g_latRender, &r50, &r99, &rn);
                    note_log("[notepad] render_lat n=%d p50=%uus p99=%uus\n",
                             rn, (unsigned)r50, (unsigned)r99);
                }
            }
        }
        /* 输入优先: 渲染期间到达的按键已经在环里了 —— 立刻再走一轮,
           不要先 yield 一整圈 (原来那次 yield 会把按键拖到下一轮, 用户体感
           就是"打完字才一次性出现")。环空才让出 CPU。 */
        if (__atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE) != cursor)
            continue;
        sys_yield();   /* event loop: stay cheap, text editing is not 60 Hz */
    }
    return 0;
}
