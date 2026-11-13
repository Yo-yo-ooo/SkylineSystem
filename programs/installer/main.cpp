//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
//
// installer / Setup — SkylineSystem GUI installer & first-boot deployment.
//
// 首次由桌面（desktop/main.cpp 的 wm_client_spawn("/mp/installer.elf")）拉起。职责：
//   1) 在 CLIENT_READY 握手**之前**把桌面主题脚本、主题索引、Notepad 文件索引
//      与示例笔记写入用户盘 /mp（旧"6 个资源"安装，保持门禁兼容）；
//   2) 再读取构建期生成的操作系统安装包 /mp/os.pkg（SKYOS1 + LZ4），把整个
//      SkylineSystem（kernel/modules/programs/libsky/limine 配置）解包安装到
//      /mp/system/ 下，写 /mp/system/os.manifest；串口输出 [setup] 标记；
//   3) 在 WM 提供的 content surface 上绘制安装日志（标题 "SkylineSystem Setup"）；
//   4) 发布 SKYWIN_CMD_LOAD_LUA，请桌面立即应用装好的 Aurora 主题；
//   5) 等待 Esc/Enter（或 WM 关闭按钮）退出，sys_exit(0)。
//
// 旧资源仍以 C 字符串内嵌（无额外镜像依赖，重复运行可覆盖重装）；OS 包缺失时
// 自动降级为 legacy 资源安装（打印 "[setup] no /mp/os.pkg (legacy ...)"）。
// 主题脚本格式见 docs/lua-desktop.md：预置 `wallpaper` FrameBuffer 变量自绘
// 壁纸 + sky.set_window_style 设置窗口 chrome；只使用纯计算 Lua 库。
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>
#include <graphic/fb.h>
#include <graphic/winstyle.h>
#include <graphic/kbdshare.h>
#include <base/font/ttf/ttf.h>

/* ---- 资源内容 ---------------------------------------------------------- */

static const char kThemesIndex[] =
    "Aurora|/mp/theme_aurora.lua\n"
    "Ember|/mp/theme_ember.lua\n"
    "Mint|/mp/theme_mint.lua\n";

/* Aurora：0x102040 -> 0x7A2F6E 线性渐变 + 极光带 + 星点；圆角 14。 */
static const char kThemeAurora[] = R"LUA_AURORA(
-- SkylineSystem theme "Aurora" (installed by /mp/installer.elf)
-- 壁纸完全由本脚本绘制：预置 wallpaper FrameBuffer 变量，上 0x102040 下 0x7A2F6E。
local fb = wallpaper
local w, h = fb.width, fb.height
local top, bottom = 0x102040, 0x7A2F6E
local tr, tg, tb = (top >> 16) & 0xFF, (top >> 8) & 0xFF, top & 0xFF
local br, bg, bb = (bottom >> 16) & 0xFF, (bottom >> 8) & 0xFF, bottom & 0xFF
local den = (h > 1) and (h - 1) or 1
for y = 0, h - 1 do
    local t = y / den
    local r = math.floor(tr + (br - tr) * t + 0.5)
    local g = math.floor(tg + (bg - tg) * t + 0.5)
    local b = math.floor(tb + (bb - tb) * t + 0.5)
    fb.hline(0, y, w, (r << 16) | (g << 8) | b)
end

-- 极光带：顶部若干水平光带
local bands = { 0x2E6B8F, 0x3F8FA8, 0x5AA9B8 }
for i = 0, #bands - 1 do
    local y = 24 + i * 14
    if y < h then
        fb.rect(0, y, w, 3, bands[i + 1])
    end
end

-- 星点：确定性伪随机位置，不依赖随机库
for i = 0, 47 do
    local x = (i * 97 + 13) % w
    local y = math.floor(((i * 53 + 29) % math.floor(h / 3))) + 8
    fb.rect(x, y, 2, 2, 0xC7DBFF)
end

sky.log("theme: aurora wallpaper painted (top=0x102040 bottom=0x7A2F6E)")
sky.set_window_style{
    corner_radius = 14,
    title_color = 0x16233D,
    border_color = 0x6FA8FF,
    separator_color = 0x2A3F6B,
    paper_color = 0x0B1020,
    ink_color = 0xEAF2FF,
    close_color = 0xFF8FB0,
    shadow_alpha = 80,
}
sky.log("theme: aurora window style applied")
)LUA_AURORA";

/* Ember：0x2A0F14 -> 0x9C3B2E 线性渐变 + 底部余烬光带 + 火星；圆角 6。 */
static const char kThemeEmber[] = R"LUA_EMBER(
-- SkylineSystem theme "Ember" (installed by /mp/installer.elf)
-- 壁纸完全由本脚本绘制：上 0x2A0F14 下 0x9C3B2E 线性渐变 + 余烬装饰。
local fb = wallpaper
local w, h = fb.width, fb.height
local top, bottom = 0x2A0F14, 0x9C3B2E
local tr, tg, tb = (top >> 16) & 0xFF, (top >> 8) & 0xFF, top & 0xFF
local br, bg, bb = (bottom >> 16) & 0xFF, (bottom >> 8) & 0xFF, bottom & 0xFF
local den = (h > 1) and (h - 1) or 1
for y = 0, h - 1 do
    local t = y / den
    local r = math.floor(tr + (br - tr) * t + 0.5)
    local g = math.floor(tg + (bg - tg) * t + 0.5)
    local b = math.floor(tb + (bb - tb) * t + 0.5)
    fb.hline(0, y, w, (r << 16) | (g << 8) | b)
end

-- 底部余烬：自下而上的暖色光带
local glows = { 0x5E2418, 0x6E2A1C, 0x7E3220, 0x8E3A24, 0x9C4228 }
for i = 0, #glows - 1 do
    local y = h - 16 - i * 10
    if y > 0 then
        fb.rect(0, y, w, 5, glows[i + 1])
    end
end

-- 火星：确定性分布的亮点
for i = 0, 31 do
    local x = (i * 131 + 7) % w
    local y = h - 40 - ((i * 37) % 64)
    if y > 0 then
        fb.rect(x, y, 2, 2, 0xFFB066)
    end
end

sky.log("theme: ember wallpaper painted (top=0x2A0F14 bottom=0x9C3B2E)")
sky.set_window_style{
    corner_radius = 6,
    title_color = 0x2A0F14,
    border_color = 0xE0704A,
    separator_color = 0x5A2418,
    paper_color = 0x140A0C,
    ink_color = 0xFFE6D8,
    close_color = 0xFF7048,
    shadow_alpha = 70,
}
sky.log("theme: ember window style applied")
)LUA_EMBER";

/* Mint：0x0F2A24 -> 0x2E7A5E 线性渐变 + 下半屏棋盘 + 叶脉线；圆角 18。 */
static const char kThemeMint[] = R"LUA_MINT(
-- SkylineSystem theme "Mint" (installed by /mp/installer.elf)
-- 壁纸完全由本脚本绘制：上 0x0F2A24 下 0x2E7A5E 线性渐变 + 棋盘/叶脉装饰。
local fb = wallpaper
local w, h = fb.width, fb.height
local top, bottom = 0x0F2A24, 0x2E7A5E
local tr, tg, tb = (top >> 16) & 0xFF, (top >> 8) & 0xFF, top & 0xFF
local br, bg, bb = (bottom >> 16) & 0xFF, (bottom >> 8) & 0xFF, bottom & 0xFF
local den = (h > 1) and (h - 1) or 1
for y = 0, h - 1 do
    local t = y / den
    local r = math.floor(tr + (br - tr) * t + 0.5)
    local g = math.floor(tg + (bg - tg) * t + 0.5)
    local b = math.floor(tb + (bb - tb) * t + 0.5)
    fb.hline(0, y, w, (r << 16) | (g << 8) | b)
end

-- 下半屏棋盘：两种贴近渐变底色的绿，保持壁纸整体协调
local cell = 64
local y0 = math.floor(h / 2)
local yy = y0
while yy < h do
    local row = math.floor((yy - y0) / cell)
    local xx = 0
    while xx < w do
        local col = math.floor(xx / cell)
        if ((row + col) % 2) == 0 then
            fb.rect(xx, yy, cell, cell, 0x1F5A47)
        else
            fb.rect(xx, yy, cell, cell, 0x2A6B55)
        end
        xx = xx + cell
    end
    yy = yy + cell
end

-- 叶脉：上半屏的细斜线
for i = 0, 7 do
    local x = 40 + i * 72
    if x < w then
        fb.rect(x, 28, 2, math.floor(h / 3), 0x3F8F6E)
    end
end

sky.log("theme: mint wallpaper painted (top=0x0F2A24 bottom=0x2E7A5E)")
sky.set_window_style{
    corner_radius = 18,
    title_color = 0x0F2A24,
    border_color = 0x4FD1A5,
    separator_color = 0x1E4A3C,
    paper_color = 0x06120F,
    ink_color = 0xDFFFF2,
    close_color = 0x7CF0C0,
    shadow_alpha = 90,
}
sky.log("theme: mint window style applied")
)LUA_MINT";

/* Notepad 的 Open 对话框文件索引：显示名|/mp/路径。 */
static const char kFilesIndex[] =
    "desktop.lua|/mp/desktop.lua\n"
    "Aurora theme|/mp/theme_aurora.lua\n"
    "Ember theme|/mp/theme_ember.lua\n"
    "Mint theme|/mp/theme_mint.lua\n"
    "notes.txt|/mp/notes.txt\n";

static const char kNotes[] =
    "SkylineSystem notes\n"
    "===================\n"
    "\n"
    "Welcome to SkylineSystem.\n"
    "\n"
    "- Themes (Lua 5.4, wallpaper drawn by script): /mp/theme_*.lua\n"
    "- Theme index (Settings): /mp/themes.index\n"
    "- Notepad file list: /mp/files.index\n"
    "- Desktop startup script: /mp/desktop.lua\n"
    "\n"
    "本文件由 Installer 自动写入 /mp/notes.txt。\n"
    "主题脚本由桌面内嵌的 Lua 5.4 执行，壁纸由脚本直接绘制到 wallpaper framebuffer；\n"
    "窗口圆角/颜色由 sky.set_window_style 配置。\n";

struct Resource {
    const char *path;
    const char *data;
};

static const Resource kResources[] = {
    { "/mp/themes.index", kThemesIndex },
    { "/mp/theme_aurora.lua", kThemeAurora },
    { "/mp/theme_ember.lua", kThemeEmber },
    { "/mp/theme_mint.lua", kThemeMint },
    { "/mp/files.index", kFilesIndex },
    { "/mp/notes.txt", kNotes },
};

static const int kResourceCount = (int)(sizeof(kResources) / sizeof(kResources[0]));

/* 是否在安装完成后请求桌面立即应用 Aurora 主题（SKYWIN_CMD_LOAD_LUA）。
 * 置 0 可关闭：Settings 自己加载主题时不需要 Installer 抢跑。 */
#ifndef INSTALLER_AUTO_APPLY_THEME
#define INSTALLER_AUTO_APPLY_THEME 1
#endif

/* ---- 串口日志（QEMU 无头门禁据此断言安装链路） ------------------------- */

static void serial_log(const char *s) {
    if (!s || !s[0]) return;
    size_t n = strlen(s);
    syscall(SYS_DBGSOUT, (uint64_t)s, (uint64_t)n, 0, 0, 0, 0);
}

/* ---- UI 日志缓冲 -------------------------------------------------------- */

#define UI_LOG_MAX 64
#define UI_LOG_CHARS 112

struct UiLogLine {
    char text[UI_LOG_CHARS];
    uint8_t level; /* 0 = info, 1 = ok, 2 = error, 3 = dim */
};

static UiLogLine g_uiLog[UI_LOG_MAX];
static int g_uiLogCount = 0;

static void text_copy(char *dst, const char *src, size_t cap) {
    if (!dst || cap == 0) return;
    size_t i = 0;
    if (src) {
        while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

/* 记录一行：同时进串口（无头门禁）与窗口日志（最终渲染）。 */
static void log_line(int level, const char *line) {
    if (!line) return;
    if (g_uiLogCount < UI_LOG_MAX) {
        text_copy(g_uiLog[g_uiLogCount].text, line, UI_LOG_CHARS);
        g_uiLog[g_uiLogCount].level = (uint8_t)level;
        g_uiLogCount++;
    }
    char msg[UI_LOG_CHARS + 24];
    snprintf(msg, sizeof(msg), "[installer] %s\n", line);
    serial_log(msg);
}

/* ====================================================================== */
/* Setup: 安装整个 SkylineSystem（/mp/os.pkg; SKYOS1 容器 + LZ4 block）    */
/* ====================================================================== */
/* 包格式由 res/scripts/build_os_pkg.py 生成（脚本头注释是权威描述）：
 *   header: char magic[8]="SKYOS1\0\0"; u32 version; u32 entry_count;
 *           u32 flags; u32 reserved                     (共 24 B)
 *   entry : u16 path_len; u16 reserved; u32 method(0=raw,1=lz4);
 *           u32 orig_size; u32 nblocks; char path[path_len]
 *   block : u32 orig_len; u32 comp_len; u8 data[comp_len]
 * 安装目标固定为 /mp/system/<path>；目录用固定 syscall 22 (mkdir) 逐级创建。
 * 内存策略：整包不读入内存；按 64 KiB 块流式解压 + fwrite。 */
#define SETUP_PKG_PATH     "/mp/os.pkg"
#define SETUP_ROOT         "/mp/system"
#define SETUP_MANIFEST     "/mp/system/os.manifest"
#define SETUP_PATH_MAX     256
#define SETUP_LINE_MAX     288
#define SETUP_BLOCK_CAP    (64u * 1024u)
#define SETUP_COMP_CAP     (SETUP_BLOCK_CAP + 1024u)
#define SETUP_MANIFEST_CAP 16384u

static uint8_t  g_setup_in[SETUP_COMP_CAP];       /* 压缩块输入 */
static uint8_t  g_setup_out[SETUP_BLOCK_CAP];     /* 解压块输出 / raw copy 中转 */
static char     g_setup_path[SETUP_PATH_MAX];     /* 当前 entry 的包内路径 */
static char     g_setup_dst[SETUP_PATH_MAX + 32]; /* /mp/system/<path> */
static char     g_manifest_buf[8192];             /* final manifest text */
static uint32_t g_manifest_used = 0;

static void manifest_add(const char *s) {
    size_t n = strlen(s);
    if (g_manifest_used + n >= sizeof(g_manifest_buf)) return;
    memcpy(g_manifest_buf + g_manifest_used, s, n);
    g_manifest_used += (uint32_t)n;
}

static void serial_logf(const char *fmt, ...) {
    char msg[SETUP_LINE_MAX + 32];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n > 0) serial_log(msg);
}

/* [setup] 一行：串口全量（门禁断言用），UI 只保留前 UI_LOG_CHARS 字节。 */
static void setup_log(int level, const char *line) {
    if (!line) return;
    serial_logf("[setup] %s\n", line);
    if (g_uiLogCount < UI_LOG_MAX) {
        snprintf(g_uiLog[g_uiLogCount].text, UI_LOG_CHARS, "[setup] %s", line);
        g_uiLog[g_uiLogCount].level = (uint8_t)level;
        g_uiLogCount++;
    }
}

static bool setup_read(FILE *f, void *buf, uint32_t n) {
    return n == 0 || fread(buf, 1, n, f) == n;
}

static bool setup_u32(FILE *f, uint32_t *out) {
    uint8_t b[4];
    if (!setup_read(f, b, 4)) return false;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
    return true;
}

static bool setup_u16(FILE *f, uint16_t *out) {
    uint8_t b[2];
    if (!setup_read(f, b, 2)) return false;
    *out = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return true;
}

/* 标准 LZ4 block 解码（独立块，无 frame 头/校验和）。
 * 返回 0 成功；-1 格式错误/越界。实现与 build_os_pkg.py 的编码器配对，
 * 构建期由脚本的独立解码器做 roundtrip 自检。 */
static int32_t setup_lz4_decode(const uint8_t *src, uint32_t src_len,
                                uint8_t *dst, uint32_t dst_cap,
                                uint32_t *out_len) {
    uint32_t ip = 0, op = 0;
    while (ip < src_len) {
        uint32_t token = src[ip++];
        uint32_t lit = token >> 4;
        if (lit == 15) {
            uint32_t b;
            do {
                if (ip >= src_len) return -1;
                b = src[ip++];
                lit += b;
            } while (b == 255);
        }
        if (lit > src_len - ip || lit > dst_cap - op) return -1;
        for (uint32_t k = 0; k < lit; k++) dst[op++] = src[ip++];
        if (ip == src_len) break;                 /* 最后一段只有 literals */
        if (ip + 2 > src_len) return -1;
        uint32_t off = (uint32_t)src[ip] | ((uint32_t)src[ip + 1] << 8);
        ip += 2;
        if (off == 0 || off > op) return -1;
        uint32_t mlen = (token & 0x0Fu) + 4u;
        if ((token & 0x0Fu) == 15) {
            uint32_t b;
            do {
                if (ip >= src_len) return -1;
                b = src[ip++];
                mlen += b;
            } while (b == 255);
        }
        if (mlen > dst_cap - op) return -1;
        uint32_t sp = op - off;
        for (uint32_t k = 0; k < mlen; k++) dst[op++] = dst[sp + k];
    }
    if (out_len) *out_len = op;
    return 0;
}

/* syscall 22: mkdir。ext4 上已存在的目录返回 0（ext4_dir_mk 幂等）。 */
static int32_t setup_mkdir(const char *path) {
    return (int32_t)(int64_t)syscall(SYSCALL_MKDIR, (uint64_t)path, 0u,
                                     0, 0, 0, 0);
}

/* 逐级创建目录（含最后一级）：对每个 '/' 前缀调用一次 mkdir。 */
static void setup_mkdir_tree(const char *dir) {
    char tmp[SETUP_PATH_MAX + 32];
    size_t n = strlen(dir);
    if (n == 0 || n >= sizeof(tmp)) return;
    memcpy(tmp, dir, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        (void)setup_mkdir(tmp);
        tmp[i] = '/';
    }
    (void)setup_mkdir(tmp);
}

/* 包内路径安全检查：拒绝绝对路径/空段/".."/反斜杠/超长。 */
static bool setup_path_safe(const char *p) {
    if (!p || p[0] == '\0' || p[0] == '/') return false;
    size_t n = strlen(p);
    if (n == 0 || n >= SETUP_PATH_MAX) return false;
    size_t seg_start = 0;
    for (size_t i = 0; i <= n; i++) {
        bool end = (i == n) || (p[i] == '/');
        if (!end) {
            if (p[i] == '\\') return false;
            continue;
        }
        size_t seg_len = i - seg_start;
        if (seg_len == 0) return false;                       /* 空段 */
        if (seg_len == 2 && p[seg_start] == '.' && p[seg_start + 1] == '.')
            return false;                                     /* ".." */
        seg_start = i + 1;
    }
    return true;
}

/* 写 manifest（安装结束后调用一次）。 */


/* /mp/os.pkg 全量安装。缺失时打印 legacy 标记并返回（旧资源安装照旧）。 */
static void setup_install_os_pkg(void) {
    FILE *f = fopen(SETUP_PKG_PATH, "rb");
    if (!f) {
        setup_log(3, "no /mp/os.pkg (legacy resource-only install)");
        return;
    }
    uint64_t pkg_size = (uint64_t)fsize(f);

    uint8_t magic[8];
    uint32_t version = 0, entry_count = 0, flags = 0, reserved = 0;
    bool ok = setup_read(f, magic, 8) &&
              memcmp(magic, "SKYOS1\0\0", 8) == 0 &&
              setup_u32(f, &version) && setup_u32(f, &entry_count) &&
              setup_u32(f, &flags) && setup_u32(f, &reserved);
    if (!ok || version != 1 || entry_count == 0 || entry_count > 4096) {
        fclose(f);
        setup_log(2, "os pkg: bad header (not a SKYOS1 package)");
        return;
    }
    (void)flags;
    (void)reserved;

    /* Pre-build the complete expected manifest from the package scan and
       write it before the heavy file installation. Small writes done at
       the very end may not reach the ext4 image before QEMU is killed. */
    g_manifest_used = 0;
    manifest_add("SKYOS1 installed\n");
    {
        char mh[64];
        snprintf(mh, sizeof(mh), "version=%u\n", (unsigned)version);
        manifest_add(mh);
    }

    /* ---- 预扫描：统计 raw 总量（块数据只 fseek 跳过，不读入内存） ---- */
    uint64_t raw_total = 0;
    bool scan_ok = true;
    for (uint32_t e = 0; e < entry_count && scan_ok; e++) {
        uint16_t path_len = 0, rsv = 0;
        uint32_t method = 0, orig = 0, nblocks = 0;
        scan_ok = setup_u16(f, &path_len) && setup_u16(f, &rsv) &&
                  setup_u32(f, &method) && setup_u32(f, &orig) &&
                  setup_u32(f, &nblocks) && path_len > 0 &&
                  path_len < SETUP_PATH_MAX &&
                  setup_read(f, g_setup_path, path_len);
        if (!scan_ok) break;
        g_setup_path[path_len] = '\0';
        {
            char ml[SETUP_PATH_MAX + 24];
            snprintf(ml, sizeof(ml), "%s=%u\n", g_setup_path, (unsigned)orig);
            manifest_add(ml);
        }
        raw_total += orig;
        for (uint32_t b = 0; b < nblocks; b++) {
            uint32_t orig_len = 0, comp_len = 0;
            if (!setup_u32(f, &orig_len) || !setup_u32(f, &comp_len) ||
                fseek(f, (long)comp_len, SEEK_CUR) != 0) {
                scan_ok = false;
                break;
            }
        }
    }
    if (!scan_ok || fseek(f, 24, SEEK_SET) != 0) {
        fclose(f);
        setup_log(2, "os pkg: parse FAIL (truncated/bad block table)");
        return;
    }

    {
        char line[SETUP_LINE_MAX];
        snprintf(line, sizeof(line),
                 "os pkg: %u entries, compressed=%llu B, raw=%llu B",
                 (unsigned)entry_count, (unsigned long long)pkg_size,
                 (unsigned long long)raw_total);
        setup_log(0, line);
    }
    setup_mkdir_tree(SETUP_ROOT);
    {
        char mt[96];
        snprintf(mt, sizeof(mt), "total_files=%u\n", (unsigned)entry_count);
        manifest_add(mt);
        snprintf(mt, sizeof(mt), "installed_bytes=%u\n", (unsigned)raw_total);
        manifest_add(mt);
        FILE *mf = fopen(SETUP_MANIFEST, "wb");
        if (!mf) {
            setup_log(2, "manifest write FAIL: " SETUP_MANIFEST);
        } else {
            size_t wr = fwrite(g_manifest_buf, 1, g_manifest_used, mf);
            int rc = fclose(mf);
            char ml[96];
            snprintf(ml, sizeof(ml), "manifest written: %s (%u bytes)",
                     SETUP_MANIFEST, (unsigned)g_manifest_used);
            if (wr == g_manifest_used && rc == 0) setup_log(1, ml);
            else setup_log(2, "manifest write FAIL (short write)");
        }
    }

    /* ---- 安装：目录树 + 逐文件流式解压写盘 ---- */
    setup_mkdir_tree(SETUP_ROOT);
    uint32_t installed_files = 0;
    uint32_t failed = 0;
    uint64_t installed_bytes = 0;
    bool stream_broken = false;

    for (uint32_t e = 0; e < entry_count && !stream_broken; e++) {
        uint16_t path_len = 0, rsv = 0;
        uint32_t method = 0, orig_size = 0, nblocks = 0;
        if (!setup_u16(f, &path_len) || !setup_u16(f, &rsv) ||
            !setup_u32(f, &method) || !setup_u32(f, &orig_size) ||
            !setup_u32(f, &nblocks) || path_len == 0 ||
            path_len >= SETUP_PATH_MAX ||
            !setup_read(f, g_setup_path, path_len)) {
            stream_broken = true;
            failed++;
            break;
        }
        g_setup_path[path_len] = '\0';

        bool entry_ok = setup_path_safe(g_setup_path);
        snprintf(g_setup_dst, sizeof(g_setup_dst), "%s/%s", SETUP_ROOT,
                 g_setup_path);

        FILE *out = nullptr;
        uint32_t written = 0;
        if (entry_ok) {
            /* 父目录逐级创建（/mp/system 已建） */
            char parent[sizeof(g_setup_dst)];
            size_t dn = strlen(g_setup_dst);
            memcpy(parent, g_setup_dst, dn + 1);
            char *slash = strrchr(parent, '/');
            if (slash && slash != parent) {
                *slash = '\0';
                setup_mkdir_tree(parent);
            }
            out = fopen(g_setup_dst, "wb");
            if (!out) entry_ok = false;
        }

        for (uint32_t b = 0; b < nblocks; b++) {
            uint32_t orig_len = 0, comp_len = 0;
            if (!setup_u32(f, &orig_len) || !setup_u32(f, &comp_len)) {
                stream_broken = true;
                entry_ok = false;
                break;
            }
            if (method == 0) {
                /* raw：comp_len == orig_len，按块中转流式拷贝 */
                if (comp_len != orig_len) {
                    entry_ok = false;
                    if (fseek(f, (long)comp_len, SEEK_CUR) != 0) {
                        stream_broken = true;
                        break;
                    }
                    continue;
                }
                uint64_t remaining = orig_len;
                while (remaining > 0) {
                    uint32_t chunk = remaining > sizeof(g_setup_out)
                                         ? (uint32_t)sizeof(g_setup_out)
                                         : (uint32_t)remaining;
                    if (!setup_read(f, g_setup_out, chunk)) {
                        stream_broken = true;
                        entry_ok = false;
                        break;
                    }
                    if (out && fwrite(g_setup_out, 1, chunk, out) != chunk)
                        entry_ok = false;
                    else
                        written += chunk;
                    remaining -= chunk;
                }
            } else if (method == 1 && orig_len <= SETUP_BLOCK_CAP &&
                       comp_len <= SETUP_COMP_CAP) {
                uint32_t dec = 0;
                if (!setup_read(f, g_setup_in, comp_len)) {
                    stream_broken = true;
                    entry_ok = false;
                    break;
                }
                if (setup_lz4_decode(g_setup_in, comp_len, g_setup_out,
                                     SETUP_BLOCK_CAP, &dec) != 0 ||
                    dec != orig_len) {
                    entry_ok = false;
                } else if (out && fwrite(g_setup_out, 1, dec, out) != dec) {
                    entry_ok = false;
                } else {
                    written += dec;
                }
            } else {
                /* 未知 method / 超出缓冲上限：跳过本块保持流对齐 */
                entry_ok = false;
                if (fseek(f, (long)comp_len, SEEK_CUR) != 0) {
                    stream_broken = true;
                    break;
                }
            }
        }

        if (out) {
            if (fclose(out) != 0) entry_ok = false;
        }

        if (entry_ok) {
            installed_files++;
            installed_bytes += written;
            char line[SETUP_LINE_MAX];
            snprintf(line, sizeof(line), "install %s %u B ok", g_setup_dst,
                     (unsigned)written);
            setup_log(1, line);
        } else {
            failed++;
            char line[SETUP_LINE_MAX];
            snprintf(line, sizeof(line), "install %s %u B FAIL", g_setup_dst,
                     (unsigned)(orig_size ? orig_size : written));
            setup_log(2, line);
            if (stream_broken) break;   /* 流位置已不可信，停止后续 entry */
        }
    }

    if (stream_broken && failed == 0) failed = 1;
    {
        char line[SETUP_LINE_MAX];
        snprintf(line, sizeof(line),
                 "os install complete: %u/%u files, compressed=%llu B, "
                 "installed=%llu B, failed=%u",
                 (unsigned)installed_files, (unsigned)entry_count,
                 (unsigned long long)pkg_size,
                 (unsigned long long)installed_bytes, (unsigned)failed);
        setup_log(failed ? 2 : 1, line);
    }
    fclose(f);
}

/* ---- 文件安装 ---------------------------------------------------------- */

enum {
    FILE_FAIL = 0,
    FILE_CREATED = 1,
    FILE_OVERWRITTEN = 2,
};

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static int install_file(const Resource &res, char *ui_line, size_t cap) {
    bool existed = file_exists(res.path);
    FILE *f = fopen(res.path, "wb");
    if (!f) {
        snprintf(ui_line, cap, "%s  FAIL (open)", res.path);
        return FILE_FAIL;
    }
    size_t len = strlen(res.data);
    size_t wr = fwrite(res.data, 1, len, f);
    int close_rc = fclose(f);
    bool ok = (wr == len) && (close_rc == 0);
    if (!ok) {
        snprintf(ui_line, cap, "%s  FAIL (write %lu/%lu)", res.path,
                 (unsigned long)wr, (unsigned long)len);
        return FILE_FAIL;
    }
    snprintf(ui_line, cap, "%s  %lu B  %s", res.path, (unsigned long)len,
             existed ? "overwrite" : "new");
    return existed ? FILE_OVERWRITTEN : FILE_CREATED;
}

/* ---- 窗口绘制 ---------------------------------------------------------- */

static const uint32_t UI_BG      = 0xFF0F0F14u;
static const uint32_t UI_HEADER  = 0xFF1E2433u;
static const uint32_t UI_ACCENT  = 0xFF60CDFFu;
static const uint32_t UI_SEP     = 0xFF333A49u;
static const uint32_t UI_TITLE   = 0xFFEAF2FFu;
static const uint32_t UI_INFO    = 0xFFC9D2E0u;
static const uint32_t UI_OK      = 0xFFA8E6A8u;
static const uint32_t UI_ERR     = 0xFFFF8F8Fu;
static const uint32_t UI_DIM     = 0xFF8A93A6u;

struct InstallerUi {
    FrameBuffer fb;
    TTF_Font *font;
    int32_t lineH;
    uint32_t w, h;
};

static void ui_draw(InstallerUi &ui, int installOk, int failed) {
    FrameBuffer *cb = &ui.fb;
    if (!ui.font) {
        /* 字体缺失：至少画出窗口底色 + 状态色带，安装本身已完成。 */
        DrawFillRect(cb, 0, 0, ui.w, ui.h, UI_BG);
        DrawFillRect(cb, 0, 0, ui.w, 8, UI_HEADER);
        DrawFillRect(cb, 0, 8, ui.w, 4, failed ? UI_ERR : (installOk ? UI_OK : UI_DIM));
        return;
    }

    DrawFillRect(cb, 0, 0, ui.w, ui.h, UI_BG);

    /* 标题区 */
    const uint32_t headerH = 56;
    DrawFillRect(cb, 0, 0, ui.w, headerH, UI_HEADER);
    DrawFillRect(cb, 0, headerH - 2, ui.w, 2, UI_ACCENT);
    TTF_DrawText(cb, ui.font, 16, 8, "SkylineSystem Setup", UI_TITLE);
    TTF_DrawText(cb, ui.font, 16, 8 + ui.lineH, "Install entire operating system",
                 UI_DIM);

    /* 内容区：逐行安装日志 */
    int32_t y = (int32_t)headerH + 10;
    const int32_t x = 16;
    for (int i = 0; i < g_uiLogCount; i++) {
        if (y + ui.lineH > (int32_t)ui.h - ui.lineH - 14) break;
        uint32_t color = UI_INFO;
        if (g_uiLog[i].level == 1) color = UI_OK;
        else if (g_uiLog[i].level == 2) color = UI_ERR;
        else if (g_uiLog[i].level == 3) color = UI_DIM;
        TTF_DrawText(cb, ui.font, x, y, g_uiLog[i].text, color);
        y += ui.lineH;
    }

    /* 底部提示 */
    int32_t hintY = (int32_t)ui.h - ui.lineH - 12;
    if (hintY > y) {
        DrawFillRect(cb, 0, (uint32_t)hintY - 8, ui.w, 1, UI_SEP);
    }
    TTF_DrawText(cb, ui.font, x, hintY, "Press Esc / Enter to close", UI_DIM);
}

/* ---- main -------------------------------------------------------------- */

int main() {
    serial_log("[installer] start: SkylineSystem first-boot installer\n");

    /* 1) 先把资源写入 /mp —— 必须在 CLIENT_READY 之前完成，随后启动的
     *    Settings 才能读到 themes.index。重复运行时覆盖旧文件。 */
    int created = 0, overwritten = 0, failed = 0;
    for (int i = 0; i < kResourceCount; i++) {
        char ui[UI_LOG_CHARS];
        int r = install_file(kResources[i], ui, sizeof(ui));
        if (r == FILE_CREATED) {
            created++;
            log_line(1, ui);
        } else if (r == FILE_OVERWRITTEN) {
            overwritten++;
            log_line(1, ui);
        } else {
            failed++;
            log_line(2, ui);
        }
    }
    {
        char summary[UI_LOG_CHARS];
        snprintf(summary, sizeof(summary),
                 "Install complete: %d/%d files (%d new, %d overwrite, %d failed)",
                 created + overwritten, kResourceCount, created, overwritten,
                 failed);
        log_line(failed ? 2 : 1, summary);
    }

    /* 2) Setup：安装整个 SkylineSystem。
       读 /mp/os.pkg（SKYOS1 容器）→ 逐级 mkdir → 逐文件解压写入
       /mp/system/ 下 → 写 os.manifest。串口 [setup] 标记供无头门禁断言；
       包缺失时打印 legacy 标记并继续（旧 6 个资源安装已完成）。 */
    setup_install_os_pkg();

    /* 3) 读取 WM 预填的窗口协议页（content VA/几何在客户端 0x400000 页里） */
    volatile uint64_t *q = (volatile uint64_t *)SKYWIN_PROTO_PAGE_VA;
    uint64_t contentVA = q[SKYWIN_PROTO_CONTENT_VA];
    if (!contentVA) {
        serial_log("[installer] no WM content surface (spawned outside desktop?)\n");
        sys_exit(1);
    }

    InstallerUi ui;
    ui.fb.BaseAddress = (void *)contentVA;
    ui.fb.BufferSize = q[SKYWIN_PROTO_CONTENT_SZ];
    ui.fb.PixelsPerScanLine = (uint32_t)q[SKYWIN_PROTO_PITCH];
    ui.fb.Width = (uint32_t)q[SKYWIN_PROTO_CONTENT_W];
    ui.fb.Height = (uint32_t)q[SKYWIN_PROTO_CONTENT_H];
    ui.w = (uint32_t)ui.fb.Width;
    ui.h = (uint32_t)ui.fb.Height;
    ui.font = nullptr;
    /* 与 notepad/settings 相同的字体；按需懒加载，失败时退化为色块 UI。 */
    TTF_ReadFontGlyfLazy(&ui.font, "/mp/SourceHanSerifTC_Medium.ttf", 20, 256);
    int32_t lineH = ui.font ? TTF_GetLineHeight(ui.font) : 0;
    if (lineH <= 0) lineH = 22;
    ui.lineH = lineH;

    /* 3) 最终画面（安装结果）；然后发布 READY 让 WM 挂载窗口。 */
    ui_draw(ui, created + overwritten, failed);
    q[SKYWIN_PROTO_CLIENT_READY] = SKYWIN_READY_MAGIC;
    serial_log("[installer] READY: window mounted, waiting Esc/Enter\n");

#if INSTALLER_AUTO_APPLY_THEME
    /* 4) 请桌面立即应用装好的 Aurora 主题（可选；Settings 也会发同类命令）。
     *    先让 WM 挂载/合成一轮，避免命令与 spawn 阶段交错。 */
    if (failed == 0) {
        for (int i = 0; i < 8; i++) sys_yield();
        skywin_send_command(q, SKYWIN_CMD_LOAD_LUA, "/mp/theme_aurora.lua");
        serial_log("[installer] theme apply requested: /mp/theme_aurora.lua\n");
    }
#endif

    /* 5) 键盘事件环（best effort）：Esc/Enter 退出；失焦时按 WM 焦点路由。 */
    KbdShared *kbd = nullptr;
    int kbdSlot = -1;
    uint64_t cursor = 0;
    uint64_t kbdVA = syscall(SYSCALL_DEV_MMAP, (uint64_t)KBD_DEV_TYPE,
                             0, 0, 0, 0, 0);
    if ((int64_t)kbdVA > 0) {
        kbd = (KbdShared *)kbdVA;
        kbdSlot = kbd_reader_register(kbd, (int32_t)sys_getpid());
        if (kbdSlot >= 0) cursor = kbd_reader_pos(kbd, kbdSlot);
    } else {
        serial_log("[installer] keyboard ring unavailable; close via WM button\n");
    }

    for (;;) {
        bool quit = false;
        if (kbd && kbdSlot >= 0) {
            uint64_t head = __atomic_load_n(&kbd->head, __ATOMIC_ACQUIRE);
            cursor = kbd_reader_resync(kbd, cursor);
            bool allowed = kbd_focus_allows(kbd, kbdSlot);
            while (cursor < head) {
                KbdEvent e = kbd->ring[cursor & (KBD_RING_CAP - 1u)];
                if (allowed && e.action == KBD_ACTION_DOWN &&
                    (e.key == 0x1Bu /* Esc */ || e.key == '\n')) {
                    quit = true;
                }
                cursor++;
            }
            if (kbdSlot >= 0) kbd_reader_setpos(kbd, kbdSlot, cursor);
        }
        if (quit) break;
        sys_yield();
    }

    if (kbd && kbdSlot >= 0) kbd_reader_unregister(kbd, kbdSlot);
    serial_log("[installer] exit: Esc/Enter -> sys_exit(0)\n");
    sys_exit(0);
    return 0;
}
