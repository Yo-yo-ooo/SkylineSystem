//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* sky_style.cpp — 桌面运行期窗口主题默认值（壁纸改由 Lua 直接绘制）。 */
#include "sky_style.h"

#include <graphic/winstyle.h>

extern "C" {
SkyDesktopStyle g_sky_style;
}

/* 编译期默认 = 旧桌面窗口视觉（winstyle.h 常量）。 */
extern "C" void SkyStyleReset(void)
{
    g_sky_style.win_radius        = (uint32_t)SKYWIN_RADIUS;
    g_sky_style.win_title_rgb     = SKYRGB_TITLE & 0x00FFFFFFu;
    g_sky_style.win_border_rgb    = SKYRGB_BORDER & 0x00FFFFFFu;
    g_sky_style.win_separator_rgb = SKYRGB_SEP & 0x00FFFFFFu;
    g_sky_style.win_paper_rgb     = SKYRGB_PAPER & 0x00FFFFFFu;
    g_sky_style.win_ink_rgb       = SKYRGB_INK & 0x00FFFFFFu;
    g_sky_style.win_close_rgb     = SKYRGB_CLOSE & 0x00FFFFFFu;
    g_sky_style.win_shadow_alpha  = 100u;   /* 100% = 原 0.28 */
}
