//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// explorer: SkylineSystem 文件浏览器（WM client）。
//
// 与 Settings / Notepad 同构：WM 在 spawn 前填好 0x400000 的 16-slot 协议页
// （内容区 VA / 几何），本程序只画内容区并置 CLIENT_READY；键盘取自共享
// 事件环，按 WM 的 focus_owner 做焦点门控。
//
// 数据来源是**真实目录枚举**：固定 syscall 23 `readdir`（内核 fops.cpp，
// 仅 ext4）把 /mp 下的目录项写成 "d|name\n" / "f|name\n" 文本行。因此
// Explorer 能看到 Setup 安装的 /mp/system 目录树、主题脚本、程序 ELF 与
// 用户保存的笔记，不依赖任何静态清单。内核较旧（槽 23 仍为 reserved）时
// 自动降级读 /mp/files.index，保证界面仍可用。
//
// 交互（键盘；本系统没有文件对话框/双击协议）：
//   Up/Down/PgUp/PgDn/Home/End  选择
//   Enter / Right               进入目录 / 预览文件
//   Backspace / Left            返回上级目录
//   R                           重新枚举当前目录
//   Esc                         预览时返回列表；列表时退出（WM 回收窗口）
//
// 串口标记（无头门禁用）：
//   [explorer] start content=WxH cwd=/mp
//   [explorer] readdir path=... rc=<bytes|-errno> entries=N source=syscall|index
//   [explorer] enter dir=... / preview path=... bytes=... lines=... / up path=...
//   [explorer] exit
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/fb.h>
#include <graphic/winstyle.h>
#include <graphic/kbdshare.h>
#include <base/font/ttf/ttf.h>

#define EXP_MAX_ENTRIES   96
#define EXP_NAME_MAX      96
#define EXP_PATH_MAX     256
#define EXP_LINE_MAX     256
#define EXP_BUF_SIZE   16384
#define EXP_FONT_PATH   "/mp/SourceHanSerifTC_Medium.ttf"
#define EXP_ROOT        "/mp"
#define EXP_INDEX_FILE  "/mp/files.index"
#define EXP_PREVIEW_MAX 12

struct ExpEntry {
    char     name[EXP_NAME_MAX];
    char     kind;          /* 'd' 目录 / 'f' 普通文件 / '?' 其它 */
    uint64_t size;          /* 普通文件字节数；目录与未知为 0 */
};

static ExpEntry  g_entries[EXP_MAX_ENTRIES];
static int       g_count       = 0;
static int       g_sel         = 0;
static int       g_scroll      = 0;
static char      g_cwd[EXP_PATH_MAX] = EXP_ROOT;
static char      g_status[192];
static int       g_status_kind = 0;   /* 0=info 1=ok 2=warn */
static char      g_source[16]  = "syscall";
static bool      g_quit        = false;
static bool      g_preview     = false;
static char      g_previewPath[EXP_PATH_MAX];
static char      g_previewLines[EXP_PREVIEW_MAX][EXP_LINE_MAX];
static int       g_previewLineCount = 0;
static volatile uint64_t *g_q = nullptr;

/* readdir 的用户态输出缓冲：静态分配，避免 16 KiB 落在栈上。 */
static char g_dirBuf[EXP_BUF_SIZE];

/* ---- 小工具 ------------------------------------------------------------- */
static void str_copy_bounded(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        for (; i + 1u < cap && src[i]; i++) dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 串口诊断：无头门禁完全依赖这些标记判断目录枚举是否真的发生。 */
static void explorer_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
        syscall(SYS_DBGSOUT, (uint64_t)(uintptr_t)buf, (uint64_t)n,
                0, 0, 0, 0);
    }
}

static void set_status(int kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    g_status_kind = kind;
}

static char *str_trim(char *s)
{
    if (!s) return s;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) {
        char *d = s;
        while (*p) *d++ = *p++;
        *d = '\0';
    }
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') s[--n] = '\0';
        else break;
    }
    return s;
}

/* 拼接 "dir/name"；dir 末尾的 '/' 归一化，绝不写越界。 */
static void join_path(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);
    while (n > 1 && dir[n - 1] == '/') n--;
    size_t i = 0;
    for (; i < n && i + 1u < cap; i++) out[i] = dir[i];
    if (i + 1u < cap) out[i++] = '/';
    for (size_t j = 0; name[j] && i + 1u < cap; j++) out[i++] = name[j];
    out[i] = '\0';
}

/* 上级目录：截掉最后一段。不允许越过 /mp（挂载点之外没有可用 VFS 路径）。 */
static void parent_path(char *out, size_t cap, const char *path)
{
    str_copy_bounded(out, cap, path);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/') out[--n] = '\0';
    const char *slash = nullptr;
    for (size_t i = 0; i < n; i++)
        if (out[i] == '/') slash = out + i;
    if (slash && (size_t)(slash - out) >= strlen(EXP_ROOT)) {
        out[(size_t)(slash - out)] = '\0';
    } else {
        str_copy_bounded(out, cap, EXP_ROOT);
    }
}

static uint64_t query_file_size(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    uint64_t sz = (uint64_t)fsize(f);
    fclose(f);
    return sz;
}

static void format_size(uint64_t sz, char *out, size_t cap)
{
    if (sz >= 1024ull * 1024ull)
        snprintf(out, cap, "%llu.%llu MiB",
                 (unsigned long long)(sz >> 20),
                 (unsigned long long)(((sz & 0xFFFFFull) * 10ull) >> 20));
    else if (sz >= 1024ull)
        snprintf(out, cap, "%llu.%llu KiB",
                 (unsigned long long)(sz >> 10),
                 (unsigned long long)(((sz & 0x3FFull) * 10ull) >> 10));
    else
        snprintf(out, cap, "%llu B", (unsigned long long)sz);
}

/* ---- 目录枚举 ----------------------------------------------------------- */
/* 解析 readdir 的文本输出。每行 "<kind>|<name>\n"。坏行直接跳过。 */
static void parse_listing(const char *buf, size_t bytes)
{
    g_count = 0;
    size_t i = 0;
    while (i < bytes && g_count < EXP_MAX_ENTRIES) {
        size_t e = i;
        while (e < bytes && buf[e] != '\n' && buf[e] != '\0') e++;
        if (e > i + 2u && buf[i + 1] == '|') {
            const char kind = buf[i];
            const char *name = buf + i + 2u;
            size_t len = e - (i + 2u);
            if (len >= EXP_NAME_MAX) len = EXP_NAME_MAX - 1u;
            ExpEntry *ent = &g_entries[g_count];
            for (size_t k = 0; k < len; k++) ent->name[k] = name[k];
            ent->name[len] = '\0';
            ent->kind = (kind == 'd' || kind == 'f') ? kind : '?';
            ent->size = 0;
            if (ent->name[0]) g_count++;
        }
        if (e >= bytes || buf[e] == '\0') break;
        i = e + 1u;
    }
}

/* 降级路径：老内核没有槽 23 时读 /mp/files.index（"显示名|/mp/路径"）。 */
static void load_fallback_index(void)
{
    FILE *f = fopen(EXP_INDEX_FILE, "r");
    if (!f) {
        set_status(2, "readdir unavailable and no %s", EXP_INDEX_FILE);
        return;
    }
    g_count = 0;
    char line[EXP_LINE_MAX];
    while (g_count < EXP_MAX_ENTRIES && fgets(line, (int)sizeof(line), f)) {
        str_trim(line);
        if (line[0] == '\0' || line[0] == '#') continue;
        char *bar = strchr(line, '|');
        if (!bar) continue;
        const char *path = str_trim(bar + 1);
        const char *slash = nullptr;
        for (const char *p = path; *p; p++)
            if (*p == '/') slash = p;
        const char *base = slash ? slash + 1 : path;
        if (!base[0]) continue;
        ExpEntry *ent = &g_entries[g_count];
        str_copy_bounded(ent->name, EXP_NAME_MAX, base);
        ent->kind = 'f';
        ent->size = query_file_size(path);
        g_count++;
    }
    fclose(f);
    str_copy_bounded(g_source, sizeof(g_source), "index");
    set_status(2, "Legacy kernel: showing %s (%d items)", EXP_INDEX_FILE, g_count);
}

/* 目录优先 + 名称升序的插入排序；".." 单独固定在第 0 行。 */
static void sort_entries(void)
{
    for (int i = 1; i < g_count; i++) {
        ExpEntry t = g_entries[i];
        int j = i - 1;
        while (j >= 0) {
            const ExpEntry *a = &g_entries[j];
            const bool aDir = (a->kind == 'd');
            const bool tDir = (t.kind == 'd');
            bool greater = (aDir != tDir) ? (!aDir && tDir)
                                          : (strcmp(a->name, t.name) > 0);
            if (!greater) break;
            g_entries[j + 1] = g_entries[j];
            j--;
        }
        g_entries[j + 1] = t;
    }
}

static void load_dir(const char *path)
{
    str_copy_bounded(g_cwd, sizeof(g_cwd), path);
    g_sel = 0;
    g_scroll = 0;
    g_preview = false;
    g_count = 0;

    for (uint32_t i = 0; i < EXP_BUF_SIZE; i++) g_dirBuf[i] = 0;
    explorer_log("[explorer] readdir: call path=%s cap=%u\n",
                 g_cwd, (unsigned)EXP_BUF_SIZE);
    int64_t rc = (int64_t)syscall(SYSCALL_READDIR,
                                  (uint64_t)(uintptr_t)g_cwd,
                                  (uint64_t)(uintptr_t)g_dirBuf,
                                  (uint64_t)EXP_BUF_SIZE,
                                  0, 0, 0);
    if (rc < 0) {
        /* 老内核（槽 23 仍 reserved）或该路径不可枚举：退回静态清单，
           并把 cwd 固定回 /mp，避免显示一个实际没打开成功的目录。 */
        load_fallback_index();
        str_copy_bounded(g_cwd, sizeof(g_cwd), EXP_ROOT);
    } else {
        parse_listing(g_dirBuf, (size_t)rc);
        str_copy_bounded(g_source, sizeof(g_source), "syscall");
        /* 文件大小由用户态按需查询：内核只给名字/类型，避免目录枚举路径
           为每个 inode 额外做一次 open（也避免动 ext4 挂载锁的临界区）。 */
        for (int i = 0; i < g_count; i++) {
            if (g_entries[i].kind != 'f') continue;
            char full[EXP_PATH_MAX];
            join_path(full, sizeof(full), g_cwd, g_entries[i].name);
            g_entries[i].size = query_file_size(full);
        }
    }

    sort_entries();

    /* 非根目录插入 ".."（目录优先排序后仍固定在最前，方便返回）。 */
    if (strcmp(g_cwd, EXP_ROOT) != 0 && g_count < EXP_MAX_ENTRIES) {
        for (int i = g_count; i > 0; i--) g_entries[i] = g_entries[i - 1];
        ExpEntry *up = &g_entries[0];
        str_copy_bounded(up->name, EXP_NAME_MAX, "..");
        up->kind = 'd';
        up->size = 0;
        g_count++;
    }
    /* 状态栏用**含 ".." 的最终条目数**，与标题栏 "N items" 保持一致
       （".." 是插入项，放在枚举之后才算得进去）。 */
    if (strcmp(g_source, "syscall") == 0)
        set_status(1, "%d item(s) - %s", g_count, g_cwd);
    explorer_log("[explorer] readdir path=%s rc=%lld entries=%d source=%s\n",
                 g_cwd, (long long)rc, g_count, g_source);
}

static void enter_entry(int idx)
{
    if (idx < 0 || idx >= g_count) return;
    const ExpEntry *e = &g_entries[idx];

    if (strcmp(e->name, "..") == 0) {
        char parent[EXP_PATH_MAX];
        parent_path(parent, sizeof(parent), g_cwd);
        explorer_log("[explorer] up path=%s\n", parent);
        load_dir(parent);
        return;
    }

    char full[EXP_PATH_MAX];
    join_path(full, sizeof(full), g_cwd, e->name);

    if (e->kind == 'd') {
        explorer_log("[explorer] enter dir=%s\n", full);
        load_dir(full);
        return;
    }

    /* ---- 普通文件：文本预览（最多 2 KiB / EXP_PREVIEW_MAX 行） ---- */
    str_copy_bounded(g_previewPath, sizeof(g_previewPath), full);
    g_previewLineCount = 0;
    for (int i = 0; i < EXP_PREVIEW_MAX; i++) g_previewLines[i][0] = '\0';

    FILE *f = fopen(full, "r");
    if (!f) {
        set_status(2, "Cannot open: %s", e->name);
        explorer_log("[explorer] preview path=%s open=failed\n", full);
        return;
    }
    char data[2049];
    size_t n = fread(data, 1, 2048, f);
    fclose(f);
    data[n] = '\0';

    /* ELF / 二进制不做乱码预览，给出明确提示 + 大小。 */
    bool binary = (n >= 4 && (uint8_t)data[0] == 0x7F &&
                   data[1] == 'E' && data[2] == 'L' && data[3] == 'F');
    if (!binary) {
        uint32_t nul = 0;
        for (size_t i = 0; i < n; i++)
            if (data[i] == '\0') nul++;
        if (nul > 0) binary = true;
    }
    if (binary) {
        char sz[32];
        format_size(e->size, sz, sizeof(sz));
        snprintf(g_previewLines[0], EXP_LINE_MAX,
                 "(binary file - %s, text preview skipped)", sz);
        g_previewLineCount = 1;
    } else {
        size_t p = 0;
        while (p < n && g_previewLineCount < EXP_PREVIEW_MAX) {
            size_t e2 = p;
            while (e2 < n && data[e2] != '\n' && data[e2] != '\r') e2++;
            size_t len = e2 - p;
            if (len >= EXP_LINE_MAX) len = EXP_LINE_MAX - 1u;
            char *dst = g_previewLines[g_previewLineCount];
            for (size_t k = 0; k < len; k++)
                dst[k] = (data[p + k] == '\t') ? ' ' : data[p + k];
            dst[len] = '\0';
            g_previewLineCount++;
            /* 跳过本行的 CR/LF（兼容 CRLF 文本），下一行从头开始。 */
            p = e2;
            while (p < n && (data[p] == '\r' || data[p] == '\n')) p++;
        }
        if (g_previewLineCount == 0) {
            str_copy_bounded(g_previewLines[0], EXP_LINE_MAX, "(empty file)");
            g_previewLineCount = 1;
        }
    }
    g_preview = true;
    set_status(1, "Preview: %s", e->name);
    explorer_log("[explorer] preview path=%s bytes=%llu lines=%d\n",
                 full, (unsigned long long)n, g_previewLineCount);
}

/* ---- 渲染 --------------------------------------------------------------- */
/* 按像素宽度裁剪后绘制，防止长文件名画穿内容区右边界。 */
static void draw_text_fit(FrameBuffer *cb, TTF_Font *font, int32_t x, int32_t y,
                          const char *text, uint32_t color, int32_t max_w)
{
    if (!font || !text || !text[0] || max_w <= 0) return;
    char buf[EXP_LINE_MAX];
    str_copy_bounded(buf, sizeof(buf), text);
    int32_t w = 0, h = 0;
    size_t n = strlen(buf);
    while (n > 0) {
        TTF_GetTextSize(font, buf, &w, &h);
        if (w <= max_w) break;
        buf[--n] = '\0';
    }
    if (n > 0) TTF_DrawText(cb, font, x, y, buf, color);
}

static void render(FrameBuffer *cb, TTF_Font *font, int32_t lineH,
                   uint32_t contentW, uint32_t contentH)
{
    const uint32_t PAD     = 16u;
    const uint32_t PAPER   = 0xFF0F0F12u;
    const uint32_t INK     = 0xFFE9E9EFu;
    const uint32_t DIM     = 0xFF9AA0A8u;
    const uint32_t MUTED   = 0xFF6E7681u;
    const uint32_t ACCENT  = 0xFF60CDFFu;
    const uint32_t DIRC    = 0xFFF0C674u;
    const uint32_t HILITE  = 0xFF1E2A36u;
    const uint32_t SEP     = 0xFF2E2E33u;
    const uint32_t BAR     = 0xFF1A1E26u;
    const uint32_t OK_C    = 0xFF8FE388u;
    const uint32_t WARN_C  = 0xFFF0C674u;

    DrawFillRect(cb, 0, 0, contentW, contentH, PAPER);
    const int32_t textMaxW = (int32_t)contentW - 2 * (int32_t)PAD;
    char line[EXP_LINE_MAX];
    int y = (int)PAD;

    /* 标题栏（内容区内自绘，不与 WM 的窗口 chrome 冲突） */
    if (font) TTF_DrawText(cb, font, (int32_t)PAD, y, "Explorer", ACCENT);
    snprintf(line, sizeof(line), "%d items  [%s]", g_count, g_source);
    if (font) {
        int32_t tw = 0, th = 0;
        TTF_GetTextSize(font, line, &tw, &th);
        if ((int32_t)contentW - (int32_t)PAD - tw > (int32_t)PAD + 100)
            draw_text_fit(cb, font, (int32_t)contentW - (int32_t)PAD - tw, y,
                          line, MUTED, textMaxW);
    }
    y += lineH + 4;
    DrawFillRect(cb, PAD, (uint64_t)y, contentW - 2u * PAD, 1u, SEP);
    y += 8;

    /* 路径栏 */
    if ((int)contentH >= 200) {
        DrawFillRect(cb, PAD, (uint64_t)y, contentW - 2u * PAD,
                     (uint64_t)lineH + 4u, BAR);
        DrawFillRect(cb, PAD, (uint64_t)y, 3u, (uint64_t)lineH + 4u, ACCENT);
        snprintf(line, sizeof(line), " %s", g_cwd);
        draw_text_fit(cb, font, (int32_t)PAD + 10, y + 2, line, INK,
                      textMaxW - 20);
        y += lineH + 12;
    }

    if ((int)contentH < 240) {
        draw_text_fit(cb, font, (int32_t)PAD, y,
                      "Up/Down Enter: open   Backspace: up   Esc: quit",
                      DIM, textMaxW);
        return;
    }

    const int footerH = lineH * 2 + 12;
    int listTop = y;
    int listBottom = (int)contentH - (int)PAD - footerH;
    if (listBottom < listTop) listBottom = listTop;
    int rows = (listBottom - listTop) / (lineH > 0 ? lineH : 1);
    if (rows < 1) rows = 1;

    if (g_preview) {
        snprintf(line, sizeof(line), "%s", g_previewPath);
        draw_text_fit(cb, font, (int32_t)PAD, listTop, line, ACCENT, textMaxW);
        int py = listTop + lineH + 4;
        for (int i = 0; i < g_previewLineCount; i++) {
            if (py + lineH > listBottom) break;
            snprintf(line, sizeof(line), "  %s", g_previewLines[i]);
            draw_text_fit(cb, font, (int32_t)PAD, py, line, DIM, textMaxW);
            py += lineH;
        }
    } else if (g_count == 0) {
        draw_text_fit(cb, font, (int32_t)PAD, listTop,
                      "(empty directory or readdir unavailable)", WARN_C,
                      textMaxW);
    } else {
        if (g_sel < g_scroll) g_scroll = g_sel;
        if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
        if (g_scroll > g_count - rows) g_scroll = g_count - rows;
        if (g_scroll < 0) g_scroll = 0;

        for (int r = 0; r < rows; r++) {
            int i = g_scroll + r;
            if (i >= g_count) break;
            int ry = listTop + r * lineH;
            const bool sel = (i == g_sel);
            if (sel)
                DrawFillRect(cb, PAD - 6u, (uint64_t)(ry - 3),
                             contentW - 2u * (PAD - 6u),
                             (uint64_t)lineH, HILITE);
            const ExpEntry *e = &g_entries[i];
            if (e->kind == 'd')
                snprintf(line, sizeof(line), "%c [DIR]  %s", sel ? '>' : ' ',
                         e->name);
            else
                snprintf(line, sizeof(line), "%c  FILE  %s", sel ? '>' : ' ',
                         e->name);
            draw_text_fit(cb, font, (int32_t)PAD, ry, line,
                          e->kind == 'd' ? (sel ? DIRC : DIM) : (sel ? INK : DIM),
                          textMaxW - 110);

            /* 右列：大小（只对普通文件），留出足够余量避免压到名称 */
            if (e->kind == 'f' && font) {
                char sz[32];
                format_size(e->size, sz, sizeof(sz));
                int32_t sw = 0, sh = 0;
                TTF_GetTextSize(font, sz, &sw, &sh);
                int32_t sx = (int32_t)contentW - (int32_t)PAD - sw;
                if (sx > (int32_t)PAD + 200)
                    TTF_DrawText(cb, font, sx, ry, sz, MUTED);
            }
        }
    }

    /* 底部：分隔线 + 快捷键 + 状态 */
    int fy = listBottom + 4;
    DrawFillRect(cb, PAD, (uint64_t)fy, contentW - 2u * PAD, 1u, SEP);
    fy += 8;
    draw_text_fit(cb, font, (int32_t)PAD, fy,
                  g_preview
                      ? "Esc: back to list    R: reload directory"
                      : "Up/Down: select   Enter: open   Backspace: up   R: reload",
                  DIM, textMaxW);
    fy += lineH;
    if (g_status[0])
        draw_text_fit(cb, font, (int32_t)PAD, fy, g_status,
                      g_status_kind == 2 ? WARN_C
                                         : (g_status_kind == 1 ? OK_C : DIM),
                      textMaxW);
}

/* ---- 键盘 --------------------------------------------------------------- */
static bool handle_key(uint16_t key)
{
    if (g_preview) {
        if (key == 0x1B) {          /* Esc：预览 -> 列表（不退出程序） */
            g_preview = false;
            set_status(1, "Back to %s", g_cwd);
            return true;
        }
        if (key == 'r' || key == 'R') {
            load_dir(g_cwd);
            return true;
        }
        return false;
    }

    switch (key) {
    case 0x1B:                      /* Esc：退出（WM 自动回收窗口） */
        g_quit = true;
        return false;
    case KBD_KEY_UP:
        if (g_count > 0) g_sel = (g_sel + g_count - 1) % g_count;
        return true;
    case KBD_KEY_DOWN:
        if (g_count > 0) g_sel = (g_sel + 1) % g_count;
        return true;
    case KBD_KEY_PGUP:
        g_sel -= 5; if (g_sel < 0) g_sel = 0;
        return true;
    case KBD_KEY_PGDN:
        g_sel += 5; if (g_sel >= g_count) g_sel = g_count > 0 ? g_count - 1 : 0;
        return true;
    case KBD_KEY_HOME:
        g_sel = 0;
        return true;
    case KBD_KEY_END:
        if (g_count > 0) g_sel = g_count - 1;
        return true;
    case '\n':
    case '\r':
    case KBD_KEY_RIGHT:
        enter_entry(g_sel);
        return true;
    case 0x08:                      /* Backspace */
    case KBD_KEY_LEFT: {
        char parent[EXP_PATH_MAX];
        parent_path(parent, sizeof(parent), g_cwd);
        if (strcmp(parent, g_cwd) != 0) {
            explorer_log("[explorer] up path=%s\n", parent);
            load_dir(parent);
        }
        return true;
    }
    case 'r':
    case 'R':
        load_dir(g_cwd);
        set_status(1, "Reloaded %d item(s)", g_count);
        return true;
    default:
        return false;
    }
}

int main()
{
    /* 第一条日志必须在任何依赖之前：spawn 失败/卡顿时用它区分
       "进程没起来" 与 "起来后卡在字体/协议页/文件系统"。 */
    explorer_log("[explorer] boot\n");

    /* WM 在启动前已填充协议页：内容区 VA + 几何（不握手）。 */
    g_q = (volatile uint64_t *)SKYWIN_PROTO_PAGE_VA;
    uint64_t contentVA = g_q[SKYWIN_PROTO_CONTENT_VA];
    uint32_t pitch     = (uint32_t)g_q[SKYWIN_PROTO_PITCH];
    uint32_t contentW  = (uint32_t)g_q[SKYWIN_PROTO_CONTENT_W];
    uint32_t contentH  = (uint32_t)g_q[SKYWIN_PROTO_CONTENT_H];
    if (!contentVA || !contentW || !contentH || !pitch) {
        explorer_log("[explorer] boot: invalid protocol page\n");
        return 1;
    }

    uint64_t kbdVA = syscall(SYSCALL_DEV_MMAP, (uint64_t)KBD_DEV_TYPE,
                             0, 0, 0, 0, 0);
    if ((int64_t)kbdVA <= 0) {
        explorer_log("[explorer] boot: kbd mmap failed rc=%lld\n",
                     (long long)(int64_t)kbdVA);
        return 1;
    }
    KbdShared *kbd = (KbdShared *)kbdVA;
    int kbdSlot = kbd_reader_register(kbd, (int32_t)sys_getpid());
    uint64_t cursor = kbdSlot >= 0 ? kbd_reader_pos(kbd, kbdSlot) : 0;

    explorer_log("[explorer] start content=%ux%u cwd=%s\n",
                 contentW, contentH, g_cwd);

    TTF_Font *font = nullptr;
    TTF_ReadFontGlyfLazy(&font, EXP_FONT_PATH, 20, 256);
    int32_t lineH = font ? TTF_GetLineHeight(font) : 22;
    if (lineH <= 0) lineH = 22;

    FrameBuffer cb;
    cb.BaseAddress       = (void *)contentVA;
    cb.BufferSize        = g_q[SKYWIN_PROTO_CONTENT_SZ];
    cb.PixelsPerScanLine = pitch;
    cb.Width             = contentW;
    cb.Height            = contentH;

    load_dir(EXP_ROOT);
    render(&cb, font, lineH, contentW, contentH);
    /* 就绪屏障：初始画面已绘好，WM 可挂载窗口。 */
    g_q[SKYWIN_PROTO_CLIENT_READY] = SKYWIN_READY_MAGIC;

    for (;;) {
        uint64_t h = __atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE);
        cursor = kbd_reader_resync(kbd, cursor);
        bool changed = false;
        const bool allowed = (kbdSlot < 0) || kbd_focus_allows(kbd, kbdSlot);
        while (cursor < h) {
            KbdEvent e = kbd->ring[cursor & (KBD_RING_CAP - 1u)];
            if (allowed && e.action == KBD_ACTION_DOWN) {
                if (handle_key(e.key)) changed = true;
            }
            cursor++;
        }
        if (kbdSlot >= 0) kbd_reader_setpos(kbd, kbdSlot, cursor);
        if (g_quit) break;
        if (changed) render(&cb, font, lineH, contentW, contentH);
        sys_yield();
    }

    explorer_log("[explorer] exit\n");
    if (kbdSlot >= 0) kbd_reader_unregister(kbd, kbdSlot);
    return 0;
}
