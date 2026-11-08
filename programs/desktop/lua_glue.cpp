//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* ============================================================================
 * lua_glue.cpp —— 桌面内嵌 Lua：桌面主题脚本宿主 + wallpaper framebuffer API
 * ----------------------------------------------------------------------------
 * 在创建窗口之前调用一次 SkyLuaInit(wallpaper_fb, w, h)：
 *   1) SkyStyleReset() 恢复编译期默认**窗口**视觉；
 *   2) 创建 Lua 5.4 状态，注册 base/string/table/math/utf8/coroutine/debug；
 *   3) 把 wallpaper 位图以 `sky.wallpaper_*` 原语暴露给脚本（tight pitch ARGB）：
 *        sky.wallpaper_info()              -> addr(lightuserdata), w, h
 *        sky.wallpaper_size()              -> w, h
 *        sky.wallpaper_fill(rgb)           -> 整屏填充
 *        sky.wallpaper_hline(x,y,len,rgb) / sky.wallpaper_vline(x,y,len,rgb)
 *        sky.wallpaper_rect(x,y,w,h,rgb)   / sky.wallpaper_pixel(x,y,rgb)
 *      壁纸**完全由脚本绘制**；C++ 不解析渐变/纯色等主题参数。
 *   4) 优先加载 /mp/desktop.lua，失败回退内嵌脚本；
 *   5) 串口打印 source / 窗口样式 / 壁纸回读样本（供无头 QEMU 门禁）。
 *
 * 只暴露纯计算库与这块壁纸位图，不开放 io/os/package；脚本错误只回退默认主题，
 * 不阻断桌面启动。
 * ==========================================================================*/
#include "lua_glue.h"
#include "sky_style.h"

/* Lua 上游头是纯 C 头（无 extern "C" guard）；本 TU 是 C++，必须用
 * extern "C" 包住，否则 luaL 系列与 luaopen 系列会按 C++ mangled 符号引用。 */
#if defined(__cplusplus) && !defined(__STDC_VERSION__)
#  define __STDC_VERSION__ 201710L
#endif
extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <syscall.h>
#include <base/arch/x86_64/syscalln.h>

/* ------------------------------------------------------------------ */
/* 串口日志（SYSCALL_DBGSOUT，QEMU 无头门禁依赖这些行）                */
/* ------------------------------------------------------------------ */
static void LuaSerial(const char *text, size_t len)
{
    if (!text || len == 0u)
        return;
    syscall(SYS_DBGSOUT, (uint64_t)text, (uint64_t)len, 0, 0, 0, 0);
}

static void LuaLog(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0)
        return;
    if (n > (int)sizeof(buf))
        n = (int)sizeof(buf);
    LuaSerial(buf, (size_t)n);
}

static void LuaLogError(const char *what, const char *msg)
{
    char buf[256];
    size_t i = 0;
    const char *prefix = "[desktop] lua: error: ";
    while (prefix[i] && i + 1u < sizeof(buf)) { buf[i] = prefix[i]; i++; }
    while (msg && *msg && i + 1u < sizeof(buf)) {
        char c = *msg++;
        buf[i++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    buf[i] = '\0';
    LuaLog("%s%s\n", what ? what : "", buf);
}

/* ------------------------------------------------------------------ */
/* wallpaper framebuffer（脚本绘制目标）                                */
/* ------------------------------------------------------------------ */
static uint32_t *g_wp = nullptr;
static uint32_t  g_wp_w = 0u;
static uint32_t  g_wp_h = 0u;

static inline uint32_t wp_color(lua_State *L, int idx)
{
    lua_Integer v = luaL_checkinteger(L, idx);
    return ((uint32_t)v) & 0x00FFFFFFu;
}

static inline void wp_put(int64_t x, int64_t y, uint32_t rgb)
{
    if (!g_wp || x < 0 || y < 0 ||
        x >= (int64_t)g_wp_w || y >= (int64_t)g_wp_h)
        return;
    g_wp[(uint64_t)y * g_wp_w + (uint64_t)x] = 0xFF000000u | rgb;
}

static int sky_wallpaper_info(lua_State *L)
{
    lua_pushlightuserdata(L, (void *)g_wp);
    lua_pushinteger(L, (lua_Integer)g_wp_w);
    lua_pushinteger(L, (lua_Integer)g_wp_h);
    return 3;
}

static int sky_wallpaper_size(lua_State *L)
{
    lua_pushinteger(L, (lua_Integer)g_wp_w);
    lua_pushinteger(L, (lua_Integer)g_wp_h);
    return 2;
}

static int sky_wallpaper_fill(lua_State *L)
{
    uint32_t rgb = wp_color(L, 1);
    if (g_wp) {
        uint64_t n = (uint64_t)g_wp_w * g_wp_h;
        for (uint64_t i = 0; i < n; i++)
            g_wp[i] = 0xFF000000u | rgb;
    }
    return 0;
}

static int sky_wallpaper_pixel(lua_State *L)
{
    int64_t x = (int64_t)luaL_checkinteger(L, 1);
    int64_t y = (int64_t)luaL_checkinteger(L, 2);
    wp_put(x, y, wp_color(L, 3));
    return 0;
}

static int sky_wallpaper_hline(lua_State *L)
{
    int64_t x = (int64_t)luaL_checkinteger(L, 1);
    int64_t y = (int64_t)luaL_checkinteger(L, 2);
    int64_t len = (int64_t)luaL_checkinteger(L, 3);
    uint32_t rgb = wp_color(L, 4);
    if (len <= 0 || !g_wp || y < 0 || y >= (int64_t)g_wp_h)
        return 0;
    int64_t x0 = x < 0 ? 0 : x;
    int64_t x1 = x + len;
    if (x1 > (int64_t)g_wp_w) x1 = (int64_t)g_wp_w;
    for (int64_t i = x0; i < x1; i++)
        wp_put(i, y, rgb);
    return 0;
}

static int sky_wallpaper_vline(lua_State *L)
{
    int64_t x = (int64_t)luaL_checkinteger(L, 1);
    int64_t y = (int64_t)luaL_checkinteger(L, 2);
    int64_t len = (int64_t)luaL_checkinteger(L, 3);
    uint32_t rgb = wp_color(L, 4);
    if (len <= 0 || !g_wp || x < 0 || x >= (int64_t)g_wp_w)
        return 0;
    int64_t y0 = y < 0 ? 0 : y;
    int64_t y1 = y + len;
    if (y1 > (int64_t)g_wp_h) y1 = (int64_t)g_wp_h;
    for (int64_t i = y0; i < y1; i++)
        wp_put(x, i, rgb);
    return 0;
}

static int sky_wallpaper_rect(lua_State *L)
{
    int64_t x = (int64_t)luaL_checkinteger(L, 1);
    int64_t y = (int64_t)luaL_checkinteger(L, 2);
    int64_t w = (int64_t)luaL_checkinteger(L, 3);
    int64_t h = (int64_t)luaL_checkinteger(L, 4);
    uint32_t rgb = wp_color(L, 5);
    if (w <= 0 || h <= 0 || !g_wp)
        return 0;
    int64_t y0 = y < 0 ? 0 : y;
    int64_t y1 = y + h;
    if (y1 > (int64_t)g_wp_h) y1 = (int64_t)g_wp_h;
    int64_t x0 = x < 0 ? 0 : x;
    int64_t x1 = x + w;
    if (x1 > (int64_t)g_wp_w) x1 = (int64_t)g_wp_w;
    for (int64_t yy = y0; yy < y1; yy++)
        for (int64_t xx = x0; xx < x1; xx++)
            wp_put(xx, yy, rgb);
    return 0;
}

/* ------------------------------------------------------------------ */
/* sky.* 窗口主题 API                                                  */
/* ------------------------------------------------------------------ */
static lua_Integer opt_int(lua_State *L, int idx, const char *key,
                           lua_Integer def)
{
    lua_getfield(L, idx, key);
    lua_Integer v = lua_isinteger(L, -1) ? lua_tointeger(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static uint32_t clamp_u32(lua_Integer v, uint32_t lo, uint32_t hi)
{
    if (v < (lua_Integer)lo) return lo;
    if (v > (lua_Integer)hi) return hi;
    return (uint32_t)v;
}

static uint32_t color_field(lua_State *L, int idx, const char *key,
                            uint32_t def)
{
    lua_Integer v = opt_int(L, idx, key, (lua_Integer)def);
    return (uint32_t)v & 0x00FFFFFFu;
}

static int sky_log(lua_State *L)
{
    const char *s = luaL_checkstring(L, 1);
    LuaLog("[lua] %s\n", s ? s : "");
    return 0;
}

static int sky_set_window_style(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);

    g_sky_style.win_radius =
        clamp_u32(opt_int(L, 1, "corner_radius",
                          (lua_Integer)g_sky_style.win_radius), 0u, 20u);
    g_sky_style.win_title_rgb =
        color_field(L, 1, "title_color", g_sky_style.win_title_rgb);
    g_sky_style.win_border_rgb =
        color_field(L, 1, "border_color", g_sky_style.win_border_rgb);
    g_sky_style.win_separator_rgb =
        color_field(L, 1, "separator_color", g_sky_style.win_separator_rgb);
    g_sky_style.win_paper_rgb =
        color_field(L, 1, "paper_color", g_sky_style.win_paper_rgb);
    g_sky_style.win_ink_rgb =
        color_field(L, 1, "ink_color", g_sky_style.win_ink_rgb);
    g_sky_style.win_close_rgb =
        color_field(L, 1, "close_color", g_sky_style.win_close_rgb);
    g_sky_style.win_shadow_alpha =
        clamp_u32(opt_int(L, 1, "shadow_alpha",
                          (lua_Integer)g_sky_style.win_shadow_alpha), 0u, 100u);
    return 0;
}

static int sky_get_style(lua_State *L)
{
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)g_sky_style.win_radius);
    lua_setfield(L, -2, "corner_radius");
    lua_pushinteger(L, (lua_Integer)g_sky_style.win_title_rgb);
    lua_setfield(L, -2, "title_color");
    lua_pushinteger(L, (lua_Integer)g_sky_style.win_border_rgb);
    lua_setfield(L, -2, "border_color");
    lua_pushinteger(L, (lua_Integer)g_sky_style.win_separator_rgb);
    lua_setfield(L, -2, "separator_color");
    lua_pushinteger(L, (lua_Integer)g_sky_style.win_paper_rgb);
    lua_setfield(L, -2, "paper_color");
    lua_pushinteger(L, (lua_Integer)g_sky_style.win_ink_rgb);
    lua_setfield(L, -2, "ink_color");
    lua_pushinteger(L, (lua_Integer)g_sky_style.win_close_rgb);
    lua_setfield(L, -2, "close_color");
    lua_pushinteger(L, (lua_Integer)g_sky_style.win_shadow_alpha);
    lua_setfield(L, -2, "shadow_alpha");
    return 1;
}

/* 预置给脚本的**壁纸 FrameBuffer 变量**（`wallpaper` / `sky.wallpaper`）：
 *   wallpaper.addr        lightuserdata，ARGB 像素首地址
 *   wallpaper.addr_u64    同地址的整数形式（方便打印/调试）
 *   wallpaper.width       / height        单位：像素
 *   wallpaper.pitch_pixels / pitch_bytes  单位：像素 / 字节（tight pitch）
 *   wallpaper.bpp=4       wallpaper.format="ARGB8888"
 *   wallpaper.fill / pixel / hline / vline / rect   绘制方法（点调用）
 * 脚本在 SkyLuaInit 加载前即可直接使用该变量；C++ 只提供 framebuffer 与像素原语，
 * 不解析任何壁纸图案/颜色配置。 */
static void push_wallpaper_var(lua_State *L)
{
    lua_newtable(L);

    lua_pushlightuserdata(L, (void *)g_wp);
    lua_setfield(L, -2, "addr");
    lua_pushinteger(L, (lua_Integer)(uintptr_t)g_wp);
    lua_setfield(L, -2, "addr_u64");
    lua_pushinteger(L, (lua_Integer)g_wp_w);
    lua_setfield(L, -2, "width");
    lua_pushinteger(L, (lua_Integer)g_wp_h);
    lua_setfield(L, -2, "height");
    lua_pushinteger(L, (lua_Integer)g_wp_w);
    lua_setfield(L, -2, "pitch_pixels");
    lua_pushinteger(L, (lua_Integer)((uint64_t)g_wp_w * 4ull));
    lua_setfield(L, -2, "pitch_bytes");
    lua_pushinteger(L, (lua_Integer)((uint64_t)g_wp_w * 4ull));
    lua_setfield(L, -2, "stride");
    lua_pushinteger(L, 4);
    lua_setfield(L, -2, "bpp");
    lua_pushstring(L, "ARGB8888");
    lua_setfield(L, -2, "format");

    lua_pushcfunction(L, sky_wallpaper_fill);
    lua_setfield(L, -2, "fill");
    lua_pushcfunction(L, sky_wallpaper_pixel);
    lua_setfield(L, -2, "pixel");
    lua_pushcfunction(L, sky_wallpaper_hline);
    lua_setfield(L, -2, "hline");
    lua_pushcfunction(L, sky_wallpaper_vline);
    lua_setfield(L, -2, "vline");
    lua_pushcfunction(L, sky_wallpaper_rect);
    lua_setfield(L, -2, "rect");
    lua_pushcfunction(L, sky_wallpaper_size);
    lua_setfield(L, -2, "size");
}

static const luaL_Reg kSkyFuncs[] = {
    {"log",               sky_log},
    {"set_window_style",  sky_set_window_style},
    {"get_style",         sky_get_style},

    {"wallpaper_info",    sky_wallpaper_info},
    {"wallpaper_size",    sky_wallpaper_size},
    {"wallpaper_fill",    sky_wallpaper_fill},
    {"wallpaper_pixel",   sky_wallpaper_pixel},
    {"wallpaper_hline",   sky_wallpaper_hline},
    {"wallpaper_vline",   sky_wallpaper_vline},
    {"wallpaper_rect",    sky_wallpaper_rect},
    {NULL, NULL},
};

/* 内嵌兜底脚本：直接使用预置的 `wallpaper` FrameBuffer 变量自己画渐变
 * （C++ 不解析渐变；脚本通过 wallpaper.hline 逐行绘制）。 */
static const char kEmbeddedScript[] =
    "-- SkylineSystem embedded desktop theme (fallback; wallpaper drawn in Lua)\n"
    "sky.log('embedded desktop.lua fallback active')\n"
    "local fb = wallpaper\n"
    "local w, h = fb.width, fb.height\n"
    "sky.log('wallpaper fb: ' .. w .. 'x' .. h .. ' ' .. fb.format ..\n"
    "         ' pitch=' .. fb.pitch_bytes .. ' bpp=' .. fb.bpp)\n"
    "local top, bottom = 0x0E1B33, 0x5C2A6E\n"
    "local tr, tg, tb = (top >> 16) & 0xFF, (top >> 8) & 0xFF, top & 0xFF\n"
    "local br, bg, bb = (bottom >> 16) & 0xFF, (bottom >> 8) & 0xFF, bottom & 0xFF\n"
    "local den = (h > 1) and (h - 1) or 1\n"
    "for y = 0, h - 1 do\n"
    "  local t = y / den\n"
    "  local r = math.floor(tr + (br - tr) * t + 0.5)\n"
    "  local g = math.floor(tg + (bg - tg) * t + 0.5)\n"
    "  local b = math.floor(tb + (bb - tb) * t + 0.5)\n"
    "  fb.hline(0, y, w, (r << 16) | (g << 8) | b)\n"
    "end\n"
    "sky.set_window_style{ corner_radius=14,\n"
    "                      title_color=0x2E3550,\n"
    "                      border_color=0x6A7AB8,\n"
    "                      separator_color=0x3A4A73,\n"
    "                      paper_color=0x000000,\n"
    "                      ink_color=0xF2F5FF,\n"
    "                      close_color=0xFFD0D8,\n"
    "                      shadow_alpha=40 }\n";

/* 只注册纯计算库；不开放 io/os/package（无文件/进程/动态库能力）。 */
static void open_sandbox_libs(lua_State *L)
{
    static const struct {
        const char *name;
        lua_CFunction fn;
    } libs[] = {
        {LUA_GNAME,         luaopen_base},
        {LUA_STRLIBNAME,    luaopen_string},
        {LUA_TABLIBNAME,    luaopen_table},
        {LUA_MATHLIBNAME,   luaopen_math},
        {LUA_UTF8LIBNAME,   luaopen_utf8},
        {LUA_COLIBNAME,     luaopen_coroutine},
        {LUA_DBLIBNAME,     luaopen_debug},
    };
    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        luaL_requiref(L, libs[i].name, libs[i].fn, 1);
        lua_pop(L, 1);
    }
}

/* ------------------------------------------------------------------ */
/* 运行期重载：保留一个 lua_State，以"关闭旧状态 -> 重置 style -> 清屏 -> */
/* 建新状态 -> 执行配置脚本"的原子序列重新应用一个 .lua 文件。          */
/* ------------------------------------------------------------------ */
static lua_State *g_L = nullptr;

static void lua_clear_wallpaper(void)
{
    if (!g_wp)
        return;
    uint64_t n = (uint64_t)g_wp_w * g_wp_h;
    for (uint64_t i = 0; i < n; i++)
        g_wp[i] = 0xFF000000u;
}

static void lua_log_style(void)
{
    LuaLog("[desktop] lua: style radius=%u title=0x%06X border=0x%06X "
           "ink=0x%06X close=0x%06X paper=0x%06X shadow=%u\n",
           (unsigned)g_sky_style.win_radius,
           (unsigned)g_sky_style.win_title_rgb,
           (unsigned)g_sky_style.win_border_rgb,
           (unsigned)g_sky_style.win_ink_rgb,
           (unsigned)g_sky_style.win_close_rgb,
           (unsigned)g_sky_style.win_paper_rgb,
           (unsigned)g_sky_style.win_shadow_alpha);
}

static void lua_log_wallpaper(void)
{
    /* 回读脚本画出的 framebuffer 实际像素（不是解析配置）。 */
    uint32_t top = 0u, mid = 0u, bot = 0u;
    if (g_wp && g_wp_w && g_wp_h) {
        top = g_wp[0];
        mid = g_wp[(uint64_t)(g_wp_h / 2u) * g_wp_w + g_wp_w / 2u];
        bot = g_wp[(uint64_t)(g_wp_h - 1u) * g_wp_w + g_wp_w / 2u];
    }
    LuaLog("[desktop] lua: wallpaper: script-painted fb=%p %ux%u "
           "top=0x%06X mid=0x%06X bottom=0x%06X\n",
           (void *)g_wp, (unsigned)g_wp_w, (unsigned)g_wp_h,
           (unsigned)(top & 0xFFFFFFu), (unsigned)(mid & 0xFFFFFFu),
           (unsigned)(bot & 0xFFFFFFu));
}

/* 建立受限库 + sky.* + 预置 `wallpaper` FrameBuffer 变量。 */
static void lua_install_api(lua_State *L)
{
    open_sandbox_libs(L);

    luaL_newlib(L, kSkyFuncs);
    lua_pushstring(L, "1.0");
    lua_setfield(L, -2, "version");
    lua_setglobal(L, "sky");

    lua_getglobal(L, "sky");                 /* [sky] */
    push_wallpaper_var(L);                   /* [sky, wp] */
    lua_pushvalue(L, -1);                    /* [sky, wp, wp] */
    lua_setfield(L, -3, "wallpaper");        /* sky.wallpaper = wp; [sky, wp] */
    lua_pushvalue(L, -1);                    /* [sky, wp, wp] */
    lua_setglobal(L, "wallpaper");           /* global wallpaper = wp; [sky, wp] */
    lua_pop(L, 2);                           /* [] */
}

/* path == nullptr/"" 表示默认 /mp/desktop.lua；allow_fallback 时加载失败回退
 * 内嵌脚本（保证桌面永远有可用的壁纸/样式）。返回 0 成功，<0 失败。 */
static int lua_run_config(const char *path, bool allow_fallback)
{
    if (g_L) {
        lua_close(g_L);
        g_L = nullptr;
    }

    SkyStyleReset();
    lua_clear_wallpaper();

    g_L = luaL_newstate();
    if (!g_L) {
        LuaLogError("newstate: ", "out of memory; using compiled defaults");
        return -1;
    }
    lua_install_api(g_L);

    const char *use_path = (path && path[0]) ? path : "/mp/desktop.lua";
    const char *source = "file";
    int status = luaL_loadfile(g_L, use_path);
    if (status != LUA_OK) {
        lua_pop(g_L, 1);
        if (allow_fallback) {
            source = "embedded";
            status = luaL_loadbufferx(g_L, kEmbeddedScript,
                                      sizeof(kEmbeddedScript) - 1u,
                                      "@embedded_desktop.lua", NULL);
        }
    }

    if (status != LUA_OK) {
        const char *msg = lua_tostring(g_L, -1);
        LuaLogError("load: ", msg ? msg : "(unknown)");
        return -2;
    }
    if (lua_pcall(g_L, 0, 0, 0) != LUA_OK) {
        const char *msg = lua_tostring(g_L, -1);
        LuaLogError("run: ", msg ? msg : "(unknown)");
        return -3;
    }

    LuaLog("[desktop] lua: source=%s\n", source);
    if (path && path[0])
        LuaLog("[desktop] lua: reload file=%s source=%s\n", use_path, source);

    lua_log_style();
    lua_log_wallpaper();
    return 0;
}

extern "C" void SkyLuaInit(uint32_t *wallpaper_fb, uint32_t width,
                           uint32_t height)
{
    g_wp = wallpaper_fb;
    g_wp_w = width;
    g_wp_h = height;

    LuaLog("[desktop] lua: init (Lua %s) wallpaper_fb=%p %ux%u\n",
           LUA_VERSION_MAJOR "." LUA_VERSION_MINOR, (void *)g_wp,
           (unsigned)g_wp_w, (unsigned)g_wp_h);

    int rc = lua_run_config(nullptr, true);
    if (rc < 0)
        LuaLogError("init: ", "fallback failed; using compiled defaults");
}

extern "C" int SkyLuaLoadConfigFile(const char *path)
{
    if (!path || strncmp(path, "/mp/", 4) != 0 || strlen(path) >= 64u) {
        LuaLogError("reload: ", "invalid config path (need /mp/*.lua)");
        return -1;
    }
    return lua_run_config(path, true);
}

extern "C" int SkyLuaReloadDesktop(void)
{
    return lua_run_config(nullptr, true);
}
