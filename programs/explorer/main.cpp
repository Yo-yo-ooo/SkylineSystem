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
#include <base/prof.h>          // sky_rdtsc / SkyLat (输入延迟量化)

#define EXP_MAX_ENTRIES   96
#define EXP_NAME_MAX      96
#define EXP_PATH_MAX     256
#define EXP_LINE_MAX     256
#define EXP_BUF_SIZE   16384
#define EXP_FONT_PATH   "/mp/SourceHanSerifTC_Medium.ttf"
#define EXP_ROOT        "/mp"
#define EXP_INDEX_FILE  "/mp/files.index"
#define EXP_PREVIEW_MAX 12

/* A/B 量化开关 (P0-2): 1 = 优化前的行为 (每次变更整页重绘), 0 = 只重画变化
   行带。同一份源码编出两个版本跑同一个探针, 才能给出可信的 before/after。 */
#ifndef SKY_FULL_REDRAW
#define SKY_FULL_REDRAW 0
#endif

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
/* 局部重绘控制: 只有"纯选中移动"才允许只重画那两行, 其余 (换目录/预览/
   重载/状态文本变化) 一律整页重绘。默认整页, 是最保守的取值。 */
static bool      g_dmgFull     = true;
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
    g_dmgFull = true;          /* 目录内容整个换了: 必须整页重绘 */

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
    g_dmgFull = true;          /* 进目录 / 预览都换掉整块内容 */

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

/* ---- 布局 ---------------------------------------------------------------
 * 把"几何 + 滚动收敛"从绘制里拆出来: 局部重绘要先用同一份布局算损伤行带,
 * 再由同一份布局绘制 —— 两条路径不可能算出不同的行位置 (原来两处各算一遍
 * 是 UI 漂移的经典来源)。
 * 行带 (band) 用法: 元素占据 [y, y+h), 与 [y0,y1) 相交才需要重绘。 */
struct ExpLayout {
    int32_t pad;
    int32_t textMaxW;
    int32_t titleY;
    int32_t sepY;
    int32_t pathY;        /* 仅 contentH >= 200 时有效 */
    int32_t hintY;        /* compact 模式的提示行 */
    int32_t listTop;
    int32_t listBottom;
    int32_t footerY;      /* 快捷键提示行的 y (状态行在 +lineH) */
    int     rows;
    bool    compact;      /* 内容区太矮: 只画一行提示 */
};

static inline bool band_hit(int32_t y0, int32_t y1, int32_t y, int32_t h)
{
    return (y < y1) && (y + h > y0);
}

static ExpLayout exp_layout(int32_t lineH, uint32_t contentW, uint32_t contentH)
{
    ExpLayout L;
    L.pad       = 16;
    L.textMaxW  = (int32_t)contentW - 2 * L.pad;
    L.compact   = false;
    L.titleY    = L.pad;
    L.sepY      = L.titleY + lineH + 4;
    L.pathY     = L.sepY + 8;
    L.hintY     = L.pathY;
    L.listTop   = L.pathY;
    L.listBottom = L.pathY;
    L.footerY   = L.pathY;
    L.rows      = 0;

    int y = L.sepY + 8;
    if ((int)contentH >= 200) y += lineH + 12;      /* 路径栏 */
    L.listTop = y;
    L.hintY   = y;
    if ((int)contentH < 240) {
        L.compact    = true;
        L.listBottom = y;
        L.footerY    = y;
        return L;
    }
    const int footerH = lineH * 2 + 12;
    L.listBottom = (int)contentH - L.pad - footerH;
    if (L.listBottom < L.listTop) L.listBottom = L.listTop;
    L.rows = (L.listBottom - L.listTop) / (lineH > 0 ? lineH : 1);
    if (L.rows < 1) L.rows = 1;
    L.footerY = L.listBottom + 4 + 8;

    /* 选中的行必须落在可见窗口内 (与原实现一致) */
    if (!g_preview && g_count > 0) {
        if (g_sel < g_scroll) g_scroll = g_sel;
        if (g_sel >= g_scroll + L.rows) g_scroll = g_sel - L.rows + 1;
        if (g_scroll > g_count - L.rows) g_scroll = g_count - L.rows;
        if (g_scroll < 0) g_scroll = 0;
    }
    return L;
}

/* 只重绘 [y0,y1) 这几行像素。
 * 为什么不是"整页重绘": 内容区 ~608x400, 一次填充 ~950 KB, 再加逐行
 * TTF_DrawText —— QEMU TCG 下单帧 10~30 ms。方向键连按时每键都整页重绘,
 * 延迟就按"键数 × 单帧"堆积。列表选中变化只需要重画"旧选中行 + 新选中行
 * + 底部状态带", 典型 3 行, 成本降两个数量级。 */
static void render_band(FrameBuffer *cb, TTF_Font *font, int32_t lineH,
                        uint32_t contentW, uint32_t contentH,
                        int32_t y0, int32_t y1)
{
    if (!cb) return;
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

    if (y0 < 0) y0 = 0;
    if (y1 > (int32_t)contentH) y1 = (int32_t)contentH;
    if (y0 >= y1) return;
    DrawFillRect(cb, 0, (uint64_t)y0, contentW, (uint64_t)(y1 - y0), PAPER);

    const ExpLayout L = exp_layout(lineH, contentW, contentH);
    const int32_t textMaxW = L.textMaxW;
    char line[EXP_LINE_MAX];

    /* 标题栏（内容区内自绘，不与 WM 的窗口 chrome 冲突） */
    if (band_hit(y0, y1, L.titleY, lineH)) {
        if (font) TTF_DrawText(cb, font, (int32_t)PAD, L.titleY, "Explorer", ACCENT);
        snprintf(line, sizeof(line), "%d items  [%s]", g_count, g_source);
        if (font) {
            int32_t tw = 0, th = 0;
            TTF_GetTextSize(font, line, &tw, &th);
            if ((int32_t)contentW - (int32_t)PAD - tw > (int32_t)PAD + 100)
                draw_text_fit(cb, font, (int32_t)contentW - (int32_t)PAD - tw,
                              L.titleY, line, MUTED, textMaxW);
        }
    }
    if (band_hit(y0, y1, L.sepY, 1))
        DrawFillRect(cb, PAD, (uint64_t)L.sepY, contentW - 2u * PAD, 1u, SEP);

    /* 路径栏 */
    if ((int)contentH >= 200 && band_hit(y0, y1, L.pathY, lineH + 4)) {
        DrawFillRect(cb, PAD, (uint64_t)L.pathY, contentW - 2u * PAD,
                     (uint64_t)lineH + 4u, BAR);
        DrawFillRect(cb, PAD, (uint64_t)L.pathY, 3u, (uint64_t)lineH + 4u, ACCENT);
        snprintf(line, sizeof(line), " %s", g_cwd);
        draw_text_fit(cb, font, (int32_t)PAD + 10, L.pathY + 2, line, INK,
                      textMaxW - 20);
    }

    if (L.compact) {
        if (band_hit(y0, y1, L.hintY, lineH))
            draw_text_fit(cb, font, (int32_t)PAD, L.hintY,
                          "Up/Down Enter: open   Backspace: up   Esc: quit",
                          DIM, textMaxW);
        return;
    }

    if (g_preview) {
        if (band_hit(y0, y1, L.listTop, lineH)) {
            snprintf(line, sizeof(line), "%s", g_previewPath);
            draw_text_fit(cb, font, (int32_t)PAD, L.listTop, line, ACCENT, textMaxW);
        }
        int py = L.listTop + lineH + 4;
        for (int i = 0; i < g_previewLineCount; i++) {
            if (py + lineH > L.listBottom) break;
            if (band_hit(y0, y1, py, lineH)) {
                snprintf(line, sizeof(line), "  %s", g_previewLines[i]);
                draw_text_fit(cb, font, (int32_t)PAD, py, line, DIM, textMaxW);
            }
            py += lineH;
        }
    } else if (g_count == 0) {
        if (band_hit(y0, y1, L.listTop, lineH))
            draw_text_fit(cb, font, (int32_t)PAD, L.listTop,
                          "(empty directory or readdir unavailable)", WARN_C,
                          textMaxW);
    } else {
        for (int r = 0; r < L.rows; r++) {
            int i = g_scroll + r;
            if (i >= g_count) break;
            int ry = L.listTop + r * lineH;
            /* 高亮块的上下各多 3px (ry-3 .. ry+lineH), 按它判相交 */
            if (!band_hit(y0, y1, ry - 3, lineH + 6)) continue;
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
    int fy = L.footerY;
    if (band_hit(y0, y1, fy - 8, 1))
        DrawFillRect(cb, PAD, (uint64_t)(fy - 8), contentW - 2u * PAD, 1u, SEP);
    if (band_hit(y0, y1, fy, lineH))
        draw_text_fit(cb, font, (int32_t)PAD, fy,
                      g_preview
                          ? "Esc: back to list    R: reload directory"
                          : "Up/Down: select   Enter: open   Backspace: up   R: reload",
                      DIM, textMaxW);
    if (g_status[0] && band_hit(y0, y1, fy + lineH, lineH))
        draw_text_fit(cb, font, (int32_t)PAD, fy + lineH, g_status,
                      g_status_kind == 2 ? WARN_C
                                         : (g_status_kind == 1 ? OK_C : DIM),
                      textMaxW);

    /* 帧提交: 与 Notepad 同理, WM 靠 OUT_SEQ 变化重合成。所有绘制路径
       (整页 render / 局部 render_band) 都收敛在这里。 */
    if (g_q)
        __atomic_add_fetch(&g_q[SKYWIN_PROTO_OUT_SEQ], 1u, __ATOMIC_RELEASE);
}

/* 整页重绘 = 行带覆盖整个内容区 (启动首帧、目录切换、预览切换用)。 */
static void render(FrameBuffer *cb, TTF_Font *font, int32_t lineH,
                   uint32_t contentW, uint32_t contentH)
{
    render_band(cb, font, lineH, contentW, contentH, 0, (int32_t)contentH);
}

/* 条目 idx 在屏幕上的行 y (不可见返回 -1)。用于算局部重绘的行带。 */
static int32_t exp_row_y(const ExpLayout &L, int32_t lineH, int idx)
{
    if (idx < 0 || idx >= g_count) return -1;
    const int r = idx - g_scroll;
    if (r < 0 || r >= L.rows) return -1;
    return L.listTop + r * lineH;
}

/* 内容区局部坐标 cy -> 条目下标 (点不中返回 -1)。鼠标点击/命中共用。 */
static int exp_row_at(int32_t lineH, uint32_t contentW, uint32_t contentH,
                      int32_t cy)
{
    if (g_preview || g_count <= 0) return -1;
    const ExpLayout L = exp_layout(lineH, contentW, contentH);
    if (L.compact) return -1;
    if (cy < L.listTop - 3 || cy >= L.listBottom) return -1;
    const int r = (cy - L.listTop) / (lineH > 0 ? lineH : 1);
    const int idx = g_scroll + r;
    if (r < 0 || r >= L.rows || idx < 0 || idx >= g_count) return -1;
    return idx;
}

/* ---- 键盘 --------------------------------------------------------------- */
static bool handle_key(uint16_t key)
{
    if (g_preview) {
        if (key == 0x1B) {          /* Esc：预览 -> 列表（不退出程序） */
            g_preview = false;
            g_dmgFull = true;
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
    g_dmgFull = false;
    /* 就绪屏障：初始画面已绘好，WM 可挂载窗口。 */
    g_q[SKYWIN_PROTO_CLIENT_READY] = SKYWIN_READY_MAGIC;

    /* 输入延迟样本: 每 32 个按键批次汇总一次 p50/p99 到串口 (P0-2 门禁用)。
       先预热 TSC 标定, 别把一次性标定时间算进第一个样本。 */
    static SkyLat lat;
    static SkyLat latRender;      /* 纯绘制耗时 (不含被抢占的时间) */
    sky_lat_reset(&lat);
    sky_lat_reset(&latRender);
    (void)sky_tsc_per_ms();

    /* 鼠标状态 (P1): WM 只在指针位于本窗口**内容区**时投递事件, 所以这里
       收到的坐标已经是 content-local, 不需要再做窗口换算。 */
    uint64_t mouseSeq  = g_q[SKYWIN_PROTO_MOUSE_SEQ];
    uint32_t mouseBtn  = 0;
    uint64_t lastClickTsc = 0;
    int      lastClickIdx = -1;

    for (;;) {
        uint64_t h = __atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE);
        cursor = kbd_reader_resync(kbd, cursor);
        bool changed = false;
        uint64_t tFirstKey = 0;         /* 本批第一个按键被取出的时刻 (TSC) */
        const int prevSel = g_sel;
        const int prevScroll = g_scroll;
        const bool allowed = (kbdSlot < 0) || kbd_focus_allows(kbd, kbdSlot);
        while (cursor < h) {
            KbdEvent e = kbd->ring[cursor & (KBD_RING_CAP - 1u)];
            if (allowed && e.action == KBD_ACTION_DOWN) {
                if (tFirstKey == 0) tFirstKey = sky_rdtsc();
                if (handle_key(e.key)) changed = true;
            }
            cursor++;
        }
        if (kbdSlot >= 0) kbd_reader_setpos(kbd, kbdSlot, cursor);
        if (g_quit) break;

        /* ---- 鼠标 (P1): 左键单击选中, 双击 (700ms 内同一行) 打开 ----
           700ms 比桌面常见的 500ms 宽: QEMU TCG 下 WM 以 30Hz 采样鼠标, 一次
           按下/抬起各要占一个采样点, 门禁用 HMP 注入时两击间隔本来就有 ~0.5s。 */
        {
            const uint64_t mseq = g_q[SKYWIN_PROTO_MOUSE_SEQ];
            if (mseq != mouseSeq) {
                mouseSeq = mseq;
                const int32_t cxm = (int32_t)g_q[SKYWIN_PROTO_MOUSE_X];
                const int32_t cym = (int32_t)g_q[SKYWIN_PROTO_MOUSE_Y];
                const uint32_t btn = (uint32_t)g_q[SKYWIN_PROTO_MOUSE_BTN];
                const bool press = ((btn & 1u) != 0) && ((mouseBtn & 1u) == 0);
                mouseBtn = btn;
                (void)cxm;   /* 目前只用行位置; 列位置留给"按列排序"等后续 */
                if (press) {
                    const int idx = exp_row_at(lineH, contentW, contentH, cym);
                    if (idx >= 0) {
                        /* 双击窗口用 TSC 量, 不用 SYSCALL_MONOTONIC_MS ——
                           实测后者的刻度与真实时间不一致 (同一次点击序列里它
                           报 3735ms, 真实只有 ~0.37s), 拿它做时间窗会永远判
                           "不是双击"。桌面主循环同样以 TSC 为时基。 */
                        const uint64_t nowTsc = sky_rdtsc();
                        const uint64_t dtMs = lastClickTsc
                            ? (nowTsc - lastClickTsc) / sky_tsc_per_ms() : 0u;
                        const bool dbl = (idx == lastClickIdx) && (dtMs <= 700u);
                        lastClickTsc = nowTsc;
                        lastClickIdx = idx;
                        g_sel = idx;
                        changed = true;
                        if (dbl) {
                            explorer_log("[explorer] mouse open idx=%d name=%s dt=%llums\n",
                                         idx, g_entries[idx].name,
                                         (unsigned long long)dtMs);
                            enter_entry(idx);          /* -> g_dmgFull */
                            lastClickIdx = -1;         /* 三击不再算双击 */
                        } else {
                            explorer_log("[explorer] mouse click idx=%d name=%s dt=%llums\n",
                                         idx, g_entries[idx].name,
                                         (unsigned long long)dtMs);
                        }
                    }
                }
            }
        }

        if (changed) {
            const uint64_t t0 = tFirstKey ? tFirstKey : sky_rdtsc();
            const uint64_t tr0 = sky_rdtsc();      /* 纯绘制耗时起点 */
            if (g_dmgFull || SKY_FULL_REDRAW) {
                render(&cb, font, lineH, contentW, contentH);
                g_dmgFull = false;
            } else {
                /* 纯选中移动: 只重画"旧选中行 ∪ 新选中行" + 底部状态带。
                   行带与 footer 带分两次画, 避免把中间那一大片也重画。 */
                const ExpLayout L = exp_layout(lineH, contentW, contentH);
                int32_t ya = exp_row_y(L, lineH, prevSel);
                int32_t yb = exp_row_y(L, lineH, g_sel);
                /* 滚动位置变了: 列表整体位移, 只重画选中行会留下上/下一行的
                   残影 —— 必须重画整个列表区 (但仍然不必重画标题/路径/页脚)。 */
                const bool listShifted = (g_scroll != prevScroll);
                if (listShifted || (ya < 0 && yb < 0)) {
                    render_band(&cb, font, lineH, contentW, contentH,
                                L.listTop - 3, L.listBottom + 3);
                } else {
                    /* 只重画"旧选中行"和"新选中行"两条行带。
                       注意**不要**用 min..max 把中间的行也带上: 一批按键可能
                       一次移动好几行, 而中间那几行的内容根本没变 (变的只是
                       高亮块), 把它们一起重画就是白白重栅格化几十个字形 ——
                       快速连按时正是这个场景 (实测 p50 会翻好几倍)。 */
                    int32_t ryOld = exp_row_y(L, lineH, prevSel);
                    int32_t ryNew = exp_row_y(L, lineH, g_sel);
                    if (ryOld >= 0)
                        render_band(&cb, font, lineH, contentW, contentH,
                                    ryOld - 3, ryOld + lineH + 3);
                    if (ryNew >= 0 && ryNew != ryOld)
                        render_band(&cb, font, lineH, contentW, contentH,
                                    ryNew - 3, ryNew + lineH + 3);
                    if (ryOld < 0 && ryNew < 0)
                        render_band(&cb, font, lineH, contentW, contentH,
                                    L.listTop - 3, L.listBottom + 3);
                }
                /* 状态行 (set_status 的结果) 在底部; 选中移动不改变它, 但
                   高亮块以外的像素必须保持, 所以只补画 footer 带。 */
                render_band(&cb, font, lineH, contentW, contentH,
                            L.footerY - 8, (int32_t)contentH);
            }
            const uint64_t renderUs = sky_tsc_to_us(sky_rdtsc() - tr0);
            if (tFirstKey != 0) {           /* 只统计键盘轮询的样本 */
                const uint64_t dt = sky_rdtsc() - t0;
                if (sky_lat_add(&lat, sky_tsc_to_us(dt))) {
                    uint32_t p50 = 0, p99 = 0;
                    int n = 0;
                    sky_lat_report(&lat, &p50, &p99, &n);
                    explorer_log("[explorer] input_lat n=%d p50=%uus p99=%uus\n",
                                 n, (unsigned)p50, (unsigned)p99);
                }
                if (sky_lat_add(&latRender, renderUs)) {
                    uint32_t r50 = 0, r99 = 0;
                    int rn = 0;
                    sky_lat_report(&latRender, &r50, &r99, &rn);
                    explorer_log("[explorer] render_lat n=%d p50=%uus p99=%uus\n",
                                 rn, (unsigned)r50, (unsigned)r99);
                }
            }
        }
        sys_yield();
    }

    explorer_log("[explorer] exit\n");
    if (kbdSlot >= 0) kbd_reader_unregister(kbd, kbdSlot);
    return 0;
}
