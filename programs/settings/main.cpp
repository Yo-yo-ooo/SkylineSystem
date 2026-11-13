//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// settings: SkylineSystem 桌面设置 / 主题加载 GUI。
//
// 职责（对应 KMOD 组件化路线的用户态验收）：
//   - 读取安装程序写出的 /mp/themes.index（每行 "显示名|/mp/xxx.lua"），
//     列出可选 Lua 主题配置；
//   - Up/Down 选择、Enter 应用：写 /mp/selected_theme.txt（格式 "显示名|路径"），
//     并通过 WM 协议页的命令邮箱发送 SKYWIN_CMD_LOAD_LUA 通知 desktop 加载；
//   - 顶部显示当前已应用主题（从 /mp/selected_theme.txt 读）；
//   - 列表为空时每 1s 自动重读 themes.index（Installer 装完自动出现）；L 手动重读；
//   - R -> SKYWIN_CMD_RELOAD_DESKTOP（重载 /mp/desktop.lua）；
//     I -> SKYWIN_CMD_SPAWN_INSTALLER（打开 Installer）；
//     Esc 退出；
//   - index 缺失时显示提示，但 I 仍可用（可先去安装主题）。
//
// 线程/进程模型与 notepad 相同：WM 在启动前填充 0x400000 的 16-slot 协议页
// （内容区 VA/几何），客户端绘制 + 发 CLIENT_READY；键盘从内核输入的共享
// 事件环读取，按 WM 的 focus_owner 做焦点门控。
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/fb.h>
#include <graphic/winstyle.h>
#include <graphic/kbdshare.h>
#include <base/font/ttf/ttf.h>

#define SET_THEME_MAX   64
#define SET_NAME_MAX    64
#define SET_PATH_MAX    96
#define SET_LINE_MAX    256
/* 命令邮箱参数区 = SKYWIN_CMD_ARG_BYTES(32) 字节，含 NUL → 路径最长 31 字节 */
#define SET_CMD_PATH_MAX (SKYWIN_CMD_ARG_BYTES - 1u)

#define SET_INDEX_FILE   "/mp/themes.index"
#define SET_SELECTED_FILE "/mp/selected_theme.txt"
#define SET_FONT_PATH    "/mp/SourceHanSerifTC_Medium.ttf"

/* ---- 主题表 + UI 状态 --------------------------------------------------- */
static char g_names[SET_THEME_MAX][SET_NAME_MAX];
static char g_paths[SET_THEME_MAX][SET_PATH_MAX];
static int  g_theme_count = 0;
static int  g_sel = 0;
static int  g_scroll = 0;
static char g_current[SET_NAME_MAX];   /* 当前已应用主题显示名；空 = 默认 */
static char g_status[192];
static int  g_status_kind = 0;         /* 0=info 1=ok 2=warn */
static bool g_quit = false;
static volatile uint64_t *g_q = nullptr;

/* ---- 小工具 ------------------------------------------------------------- */
static void str_copy_bounded(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    size_t i = 0;
    if (src) {
        for (; i + 1u < cap && src[i]; i++)
            dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 串口诊断（门禁/回归定位 selected_theme 写入语义用；无 WM 协议依赖）。 */
static void settings_log(const char *fmt, ...)
{
    char buf[240];
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

/* 去首尾空白 + CR/LF（原地，返回同一指针便于链式使用） */
static char *str_trim(char *s)
{
    if (!s)
        return s;
    char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if (p != s) {
        char *d = s;
        while (*p)
            *d++ = *p++;
        *d = '\0';
    }
    size_t n = strlen(s);
    while (n > 0) {
        char c = s[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            s[--n] = '\0';
        else
            break;
    }
    return s;
}

static void set_status(int kind, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_status, sizeof(g_status), fmt, ap);
    va_end(ap);
    g_status_kind = kind;
}

/* 按像素宽度裁剪后绘制（防止长主题名/长状态串画穿 content 区右边界，
   污染 WM 的阴影边距）。max_w <= 0 时不画。 */
static void draw_text_fit(FrameBuffer *cb, TTF_Font *font, int32_t x, int32_t y,
                          const char *text, uint32_t color, int32_t max_w)
{
    if (!text || !text[0] || max_w <= 0)
        return;
    char buf[SET_LINE_MAX];
    str_copy_bounded(buf, sizeof(buf), text);
    int32_t w = 0, h = 0;
    size_t n = strlen(buf);
    while (n > 0) {
        TTF_GetTextSize(font, buf, &w, &h);
        if (w <= max_w)
            break;
        buf[--n] = '\0';
    }
    if (n > 0)
        TTF_DrawText(cb, font, x, y, buf, color);
}

/* ---- /mp/themes.index --------------------------------------------------- */
/* 行格式: "显示名|/mp/xxx.lua"；空行/以 # 开头的行忽略，坏行跳过。 */
static void load_theme_list(void)
{
    g_theme_count = 0;
    g_sel = 0;
    g_scroll = 0;

    FILE *f = fopen(SET_INDEX_FILE, "r");
    if (!f) {
        set_status(2, "(no themes installed - press I to run Installer)");
        return;
    }
    char line[SET_LINE_MAX];
    while (g_theme_count < SET_THEME_MAX && fgets(line, (int)sizeof(line), f)) {
        str_trim(line);
        if (line[0] == '\0' || line[0] == '#')
            continue;
        char *bar = strchr(line, '|');
        if (!bar)
            continue; /* 坏行：没有分隔符 */
        *bar = '\0';
        char *name = str_trim(line);
        char *path = str_trim(bar + 1);
        if (name[0] == '\0' || path[0] == '\0')
            continue;
        str_copy_bounded(g_names[g_theme_count], SET_NAME_MAX, name);
        str_copy_bounded(g_paths[g_theme_count], SET_PATH_MAX, path);
        g_theme_count++;
    }
    fclose(f);

    if (g_theme_count == 0) {
        set_status(2, "(no themes installed - press I to run Installer)");
        return;
    }
    /* 若当前主题在列表里，默认选中它，Enter 前就能看到"当前项"。 */
    for (int i = 0; i < g_theme_count; i++) {
        if (g_current[0] && strcmp(g_names[i], g_current) == 0) {
            g_sel = i;
            break;
        }
    }
    set_status(1, "Loaded %d theme(s) from %s", g_theme_count, SET_INDEX_FILE);
    settings_log("[settings] list count=%d current=%s sel=%d\n",
                 g_theme_count, g_current[0] ? g_current : "(none)", g_sel);
}

/* ---- /mp/selected_theme.txt（本程序写入 "显示名|路径"，读取兼容旧两行） -- */
static void load_current_theme(void)
{
    g_current[0] = '\0';
    FILE *f = fopen(SET_SELECTED_FILE, "r");
    if (!f)
        return; /* 无文件 = 默认 desktop.lua */
    char line[SET_LINE_MAX];
    if (!fgets(line, (int)sizeof(line), f)) {
        fclose(f);
        return;
    }
    str_trim(line);
    char *bar = strchr(line, '|');
    if (bar) {
        *bar = '\0';
        str_trim(line);
        str_copy_bounded(g_current, sizeof(g_current), line);
    } else if (line[0]) {
        /* 兼容只写显示名的旧格式（路径行忽略；当前主题只看显示名） */
        str_copy_bounded(g_current, sizeof(g_current), line);
    }
    fclose(f);
}

static bool write_selected_theme(const char *name, const char *path)
{
    FILE *f = fopen(SET_SELECTED_FILE, "w");
    if (!f)
        return false;
    bool ok = fprintf(f, "%s|%s\n", name, path) > 0;
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

/* ---- WM 命令邮箱 -------------------------------------------------------- */
static void send_cmd(uint64_t cmd, const char *arg)
{
    skywin_send_command(g_q, cmd, arg);
}

static void apply_selected_theme(void)
{
    if (g_theme_count <= 0) {
        set_status(2, "(no themes installed - press I to run Installer)");
        return;
    }
    const char *name = g_names[g_sel];
    const char *path = g_paths[g_sel];
    const bool saved = write_selected_theme(name, path);
    settings_log("[settings] apply name=%s path=%s saved=%d sel=%d/%d\n",
                 name, path, saved ? 1 : 0, g_sel, g_theme_count);
    str_copy_bounded(g_current, sizeof(g_current), name);

    if (strlen(path) > (size_t)SET_CMD_PATH_MAX) {
        /* 协议页参数区固定 32B：超长路径无法传给 desktop，明确提示。 */
        set_status(2, "Applied %s, but path > 31 bytes cannot be sent to WM", name);
        return;
    }
    send_cmd(SKYWIN_CMD_LOAD_LUA, path);
    if (saved)
        set_status(1, "Applied: %s  (Requested: %s)", name, path);
    else
        set_status(2, "Sent LOAD_LUA, but failed to write %s", SET_SELECTED_FILE);
}

/* ---- 渲染 --------------------------------------------------------------- */
static void render(FrameBuffer *cb, TTF_Font *font, int32_t lineH,
                   uint32_t contentW, uint32_t contentH)
{
    const uint32_t PAD      = 16u;
    const uint32_t PAPER    = 0xFF0F0F12u;
    const uint32_t INK      = 0xFFE9E9EFu;
    const uint32_t DIM      = 0xFF9AA0A8u;
    const uint32_t MUTED    = 0xFF6E7681u;
    const uint32_t ACCENT   = 0xFF60CDFFu;
    const uint32_t HILITE   = 0xFF1E2A36u;
    const uint32_t SEP      = 0xFF2E2E33u;
    const uint32_t OK_C     = 0xFF8FE388u;
    const uint32_t WARN_C   = 0xFFF0C674u;

    DrawFillRect(cb, 0, 0, contentW, contentH, PAPER);

    int y = (int)PAD;
    /* 标题 */
    TTF_DrawText(cb, font, (int32_t)PAD, y, "Settings", ACCENT);
    y += lineH + 4;
    DrawFillRect(cb, PAD, (uint64_t)y, contentW - 2u * PAD, 1u, SEP);
    y += 10;

    /* 当前已应用主题 */
    char line[SET_LINE_MAX];
    const int32_t textMaxW = (int32_t)contentW - 2 * (int32_t)PAD;
    if (g_current[0])
        snprintf(line, sizeof(line), "Current theme: %s", g_current);
    else
        snprintf(line, sizeof(line), "Current theme: (default desktop.lua)");
    draw_text_fit(cb, font, (int32_t)PAD, y, line, g_current[0] ? INK : DIM,
                  textMaxW);
    y += lineH + 6;

    /* 列表标题 + 计数 */
    snprintf(line, sizeof(line), "Themes (%d)", g_theme_count);
    draw_text_fit(cb, font, (int32_t)PAD, y, line, INK, textMaxW);
    y += lineH + 4;

    /* 极窄窗口兜底：放不下 列表 + 两行快捷键 + 状态时，只画一行操作提示，
       避免列表行与底部固定区互相覆盖（正常 620x360 内容区不会走到这里）。 */
    if ((int)contentH < 240) {
        draw_text_fit(cb, font, (int32_t)PAD, y,
                      "Up/Down Enter: apply   R: reload   I: installer   Esc: quit",
                      DIM, textMaxW);
        return;
    }

    /* 底部固定区: 2 行快捷键 + 1 行状态 */
    const int footerH = lineH * 3 + 14;
    int listTop = y;
    int listBottom = (int)contentH - (int)PAD - footerH;
    if (listBottom < listTop)
        listBottom = listTop;
    int rows = (listBottom - listTop) / (lineH > 0 ? lineH : 1);
    if (rows < 1)
        rows = 1;

    if (g_theme_count == 0) {
        draw_text_fit(cb, font, (int32_t)PAD, listTop,
                      "(no themes installed - press I to run Installer)",
                      WARN_C, textMaxW);
    } else {
        /* 滚动窗口跟随当前选择 */
        if (g_sel < g_scroll)
            g_scroll = g_sel;
        if (g_sel >= g_scroll + rows)
            g_scroll = g_sel - rows + 1;
        if (g_scroll > g_theme_count - rows)
            g_scroll = g_theme_count - rows;
        if (g_scroll < 0)
            g_scroll = 0;

        for (int r = 0; r < rows; r++) {
            int i = g_scroll + r;
            if (i >= g_theme_count)
                break;
            int ry = listTop + r * lineH;
            bool sel = (i == g_sel);
            if (sel)
                DrawFillRect(cb, PAD - 6u, (uint64_t)(ry - 3),
                             contentW - 2u * (PAD - 6u), (uint64_t)lineH, HILITE);
            snprintf(line, sizeof(line), "%c %s", sel ? '>' : ' ', g_names[i]);
            draw_text_fit(cb, font, (int32_t)PAD, ry, line, sel ? INK : DIM,
                          textMaxW);
            /* 选中行右侧跟一段暗色路径（放得下才画，避免越界/压字） */
            if (sel) {
                int32_t tw = 0, th = 0;
                TTF_GetTextSize(font, line, &tw, &th);
                int32_t pw = 0, ph = 0;
                TTF_GetTextSize(font, g_paths[i], &pw, &ph);
                int32_t px = (int32_t)PAD + tw + 18;
                if (px + pw < (int32_t)contentW - (int32_t)PAD)
                    TTF_DrawText(cb, font, px, ry, g_paths[i], MUTED);
            }
        }
    }

    /* 底部: 分隔线 + 两行快捷键 + 状态行 */
    int fy = listBottom + 4;
    DrawFillRect(cb, PAD, (uint64_t)fy, contentW - 2u * PAD, 1u, SEP);
    fy += 8;
    draw_text_fit(cb, font, (int32_t)PAD, fy,
                  "Up/Down: Select    Enter: Apply", DIM, textMaxW);
    fy += lineH;
    draw_text_fit(cb, font, (int32_t)PAD, fy,
                  "R: Reload desktop  I: Installer  Esc: Quit", DIM, textMaxW);
    fy += lineH;
    if (g_status[0])
        draw_text_fit(cb, font, (int32_t)PAD, fy, g_status,
                      g_status_kind == 2 ? WARN_C
                                         : (g_status_kind == 1 ? OK_C : DIM),
                      textMaxW);
}

/* ---- 键盘 --------------------------------------------------------------- */
/* 返回 true 表示需要重绘。Esc 设置 g_quit 后主循环退出。 */
static bool handle_key(uint16_t key)
{
    switch (key) {
    case 0x1B: /* Esc（kbdshare: 可打印控制键携带 ASCII） */
        g_quit = true;
        return false;
    case KBD_KEY_UP:
        if (g_theme_count > 0)
            g_sel = (g_sel + g_theme_count - 1) % g_theme_count;
        return true;
    case KBD_KEY_DOWN:
        if (g_theme_count > 0)
            g_sel = (g_sel + 1) % g_theme_count;
        return true;
    case KBD_KEY_PGUP:
        if (g_theme_count > 0) {
            g_sel -= 5;
            if (g_sel < 0)
                g_sel = 0;
        }
        return true;
    case KBD_KEY_PGDN:
        if (g_theme_count > 0) {
            g_sel += 5;
            if (g_sel >= g_theme_count)
                g_sel = g_theme_count - 1;
        }
        return true;
    case KBD_KEY_HOME:
        g_sel = 0;
        return true;
    case KBD_KEY_END:
        if (g_theme_count > 0)
            g_sel = g_theme_count - 1;
        return true;
    case '\n':
    case '\r':
        apply_selected_theme();
        return true;
    case 'r':
    case 'R':
        send_cmd(SKYWIN_CMD_RELOAD_DESKTOP, nullptr);
        set_status(1, "Requested: reload /mp/desktop.lua");
        return true;
    case 'i':
    case 'I':
        send_cmd(SKYWIN_CMD_SPAWN_INSTALLER, nullptr);
        set_status(1, "Requested: open Installer");
        return true;
    case 'l':
    case 'L':
        /* 重新读取 themes.index / selected_theme.txt（Installer 安装完成后，
         * 或外部修改 index 后无需重启 Settings）。 */
        load_current_theme();
        load_theme_list();
        set_status(1, "Reloaded %d theme(s) from %s", g_theme_count,
                   SET_INDEX_FILE);
        return true;
    default:
        return false;
    }
}

int main()
{
    /* WM 在启动前已填充协议页：内容区 VA + 几何（不握手）。 */
    g_q = (volatile uint64_t *)SKYWIN_PROTO_PAGE_VA;
    uint64_t contentVA = g_q[SKYWIN_PROTO_CONTENT_VA];
    uint32_t pitch    = (uint32_t)g_q[SKYWIN_PROTO_PITCH];
    uint32_t contentW = (uint32_t)g_q[SKYWIN_PROTO_CONTENT_W];
    uint32_t contentH = (uint32_t)g_q[SKYWIN_PROTO_CONTENT_H];
    if (!contentVA || !contentW || !contentH || !pitch)
        return 1;

    /* 共享键盘事件环 + 本进程读者槽（WM 按点击把焦点指向该槽）。 */
    uint64_t kbdVA = syscall(SYSCALL_DEV_MMAP, (uint64_t)KBD_DEV_TYPE,
                             0, 0, 0, 0, 0);
    if ((int64_t)kbdVA <= 0)
        return 1;
    KbdShared *kbd = (KbdShared *)kbdVA;
    int kbdSlot = kbd_reader_register(kbd, (int32_t)sys_getpid());
    uint64_t cursor = kbdSlot >= 0 ? kbd_reader_pos(kbd, kbdSlot) : 0;

    /* 字体：与 notepad 相同的按需加载（只驻留小表 + 字形窗口）。 */
    TTF_Font *font = nullptr;
    TTF_ReadFontGlyfLazy(&font, SET_FONT_PATH, 20, 256);
    if (!font)
        return 1;
    int32_t lineH = TTF_GetLineHeight(font);
    if (lineH <= 0)
        lineH = 22;

    FrameBuffer cb;
    cb.BaseAddress       = (void *)contentVA;
    cb.BufferSize        = g_q[SKYWIN_PROTO_CONTENT_SZ];
    cb.PixelsPerScanLine = pitch;      /* 表面 stride（含阴影边距） */
    cb.Width             = contentW;   /* 只画内容区宽度，不碰阴影/圆角 */
    cb.Height            = contentH;

    /* 初始化数据：当前主题 -> 主题列表（预选当前项）。 */
    load_current_theme();
    load_theme_list();

    render(&cb, font, lineH, contentW, contentH);
    /* 就绪屏障：初始画面已绘好，WM 可挂载窗口。 */
    g_q[SKYWIN_PROTO_CLIENT_READY] = SKYWIN_READY_MAGIC;

    uint64_t last_empty_refresh = (uint64_t)syscall(SYSCALL_MONOTONIC_MS, 0, 0, 0, 0, 0, 0);

    for (;;) {
        uint64_t h = __atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE);
        cursor = kbd_reader_resync(kbd, cursor);
        bool changed = false;
        const bool allowed = (kbdSlot < 0) || kbd_focus_allows(kbd, kbdSlot);
        while (cursor < h) {
            KbdEvent e = kbd->ring[cursor & (KBD_RING_CAP - 1u)];
            if (allowed && e.action == KBD_ACTION_DOWN) {
                if (handle_key(e.key))
                    changed = true;
            }
            cursor++;
        }
        if (kbdSlot >= 0)
            kbd_reader_setpos(kbd, kbdSlot, cursor);
        if (g_quit)
            break;

        /* 首启时 Settings 先于 Installer 打开：若列表为空，每秒重读一次
         * themes.index，安装完成后自动显示主题（无需重启 Settings）。 */
        if (g_theme_count == 0) {
            uint64_t now = (uint64_t)syscall(SYSCALL_MONOTONIC_MS, 0, 0, 0, 0, 0, 0);
            if (now - last_empty_refresh >= 1000u) {
                last_empty_refresh = now;
                load_current_theme();
                load_theme_list();
                if (g_theme_count > 0)
                    changed = true;
            }
        }

        if (changed)
            render(&cb, font, lineH, contentW, contentH);
        sys_yield(); /* 事件驱动：空闲时让出 CPU */
    }

    if (kbdSlot >= 0)
        kbd_reader_unregister(kbd, kbdSlot);
    return 0;
}
