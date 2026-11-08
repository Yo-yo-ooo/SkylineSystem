//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
/* lua_glue.h — 桌面内嵌 Lua 初始化入口（实现见 lua_glue.cpp）。 */
#ifndef _SKY_DESKTOP_LUA_GLUE_H_
#define _SKY_DESKTOP_LUA_GLUE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 在创建窗口之前调用一次：
 *   - 注册受控的 sky.* API；
 *   - 加载 /mp/desktop.lua（缺失/出错时用内嵌脚本）；
 *   - 脚本通过 sky.wallpaper_* 原语直接绘制 wallpaper_fb（tight pitch，ARGB），
 *     通过 sky.set_window_style 设置窗口 chrome。
 * 只保留窗口主题的 C++ 默认值；壁纸完全由脚本绘制，C++ 不解析渐变/颜色配置。
 * 错误只回退默认主题并通过串口打印，不阻断桌面启动。 */
void SkyLuaInit(uint32_t *wallpaper_fb, uint32_t width, uint32_t height);

/* 运行期重新加载一个 Lua 配置/主题文件（Settings 界面通过 WM 命令触发）。
 * path 必须是 /mp/ 下的绝对路径；执行成功后 wallpaper framebuffer 与
 * g_sky_style 已被新脚本更新，调用方负责重绘窗口 chrome / 重新合成。
 * 返回 0 成功；<0 参数非法/加载失败（失败时回退内嵌默认主题）。 */
int SkyLuaLoadConfigFile(const char *path);

/* 重新加载 /mp/desktop.lua（失败回退内嵌脚本）。 */
int SkyLuaReloadDesktop(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
