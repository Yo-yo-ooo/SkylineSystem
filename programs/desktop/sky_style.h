//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* ============================================================================
 * sky_style.h —— 桌面运行期窗口主题（chrome 颜色/圆角/阴影）
 * ----------------------------------------------------------------------------
 * 编译期默认值仍在 lib/include/graphic/winstyle.h（SKYRGB_ 系列与
 * SKYWIN_RADIUS 常量）。运行期由桌面内嵌 Lua（desktop.lua）通过
 * sky.set_window_style 覆盖窗口视觉参数；未加载脚本/字段缺省时保持原值。
 *
 * **壁纸不在这里配置**：壁纸由 Lua 脚本直接画进 WallpaperFrameBuffer
 * （lua_glue.cpp 的 sky.wallpaper_* 原语）；C++ 不解析渐变/颜色之类参数。
 *
 * 只允许调整**视觉**参数：几何（窗口 body 尺寸、标题栏高度、协议页布局）
 * 保持 winstyle.h 固定，避免客户端协议/合成器布局漂移。
 * ==========================================================================*/
#ifndef _SKY_STYLE_H_
#define _SKY_STYLE_H_

#include <stdint.h>
#include <graphic/winstyle.h>

typedef struct SkyDesktopStyle {
    /* 窗口 chrome（RGB 低 24 位；alpha 恒 FF） */
    uint32_t win_radius;        /* 圆角半径 px，clamp 到 [0, 20] */
    uint32_t win_title_rgb;
    uint32_t win_border_rgb;
    uint32_t win_separator_rgb;
    uint32_t win_paper_rgb;
    uint32_t win_ink_rgb;
    uint32_t win_close_rgb;
    uint32_t win_shadow_alpha;  /* 阴影强度百分比 0..100（100 = 原 0.28） */
} SkyDesktopStyle;

#ifdef __cplusplus
extern "C" {
#endif

extern SkyDesktopStyle g_sky_style;

/* 恢复编译期默认窗口视觉 */
void SkyStyleReset(void);

#ifdef __cplusplus
} /* extern "C" */

/* 供 loader.cpp / main.cpp / wm_client.cpp 使用的取值辅助（C++ only）。
 * 颜色统一补 0xFF alpha；shadow 返回 0..0.28 的浮点系数。 */
static inline uint32_t SkyRGBTitle(void)     { return 0xFF000000u | (g_sky_style.win_title_rgb & 0x00FFFFFFu); }
static inline uint32_t SkyRGBBorder(void)    { return 0xFF000000u | (g_sky_style.win_border_rgb & 0x00FFFFFFu); }
static inline uint32_t SkyRGBSeparator(void) { return 0xFF000000u | (g_sky_style.win_separator_rgb & 0x00FFFFFFu); }
static inline uint32_t SkyRGBPaper(void)     { return 0xFF000000u | (g_sky_style.win_paper_rgb & 0x00FFFFFFu); }
static inline uint32_t SkyRGBInk(void)       { return 0xFF000000u | (g_sky_style.win_ink_rgb & 0x00FFFFFFu); }
static inline uint32_t SkyRGBClose(void)     { return 0xFF000000u | (g_sky_style.win_close_rgb & 0x00FFFFFFu); }
static inline int32_t  SkyWinRadius(void)    { return (int32_t)g_sky_style.win_radius; }
/* 固定尺寸 console 窗口的运行期内容高度（标题栏高度保持编译期常量） */
static inline int32_t  SkyWinContentH(void)  { return (int32_t)SKYWIN_H - (int32_t)SKYWIN_TITLE_H - SkyWinRadius(); }
static inline float    SkyShadowMax(void)    { return 0.28f * (float)g_sky_style.win_shadow_alpha / 100.0f; }
#endif

#endif /* _SKY_STYLE_H_ */
