-- SPDX-FileCopyrightText: 2026 Yo-yo-ooo
-- SPDX-License-Identifier: MIT
-- ============================================================================
-- desktop.lua —— SkylineSystem 桌面主题脚本（内嵌 Lua 5.4 执行）
-- ----------------------------------------------------------------------------
-- 桌面启动早期加载本文件（/mp/desktop.lua）；若不存在则使用内嵌兜底脚本。
--
-- 预置的壁纸 FrameBuffer 变量：`wallpaper`（同一个 table 也可从 sky.wallpaper 访问）
--   wallpaper.addr        lightuserdata，ARGB 像素首地址
--   wallpaper.addr_u64    同地址的整数形式
--   wallpaper.width       / height          像素
--   wallpaper.pitch_pixels / pitch_bytes    行跨度（tight pitch）
--   wallpaper.bpp = 4     wallpaper.format = "ARGB8888"
--   wallpaper.fill(rgb) / pixel(x,y,rgb) / hline(x,y,len,rgb)
--   wallpaper.vline(x,y,len,rgb) / rect(x,y,w,h,rgb) / size()
--
-- **壁纸完全由本脚本画进这个 framebuffer**（C++ 不解析渐变/颜色配置）。
-- 窗口 chrome 通过 sky.set_window_style 配置。修改后重启桌面生效。
-- ============================================================================

sky.log("desktop.lua file active")

-- 直接使用预置的壁纸 FrameBuffer 变量
local fb = wallpaper
local w, h = fb.width, fb.height
sky.log("wallpaper fb: " .. w .. "x" .. h .. " " .. fb.format ..
        " pitch=" .. fb.pitch_bytes .. " bpp=" .. fb.bpp)
sky.log("wallpaper addr = " .. fb.addr_u64)

-- ---- 壁纸：脚本自己画垂直线性渐变（顶部深蓝 -> 底部紫红）----------------
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

-- 任意绘制都由脚本负责，例如左下角色块（不影响门禁采样区域）
-- fb.rect(0, h - 180, 96, 48, 0x1A2C55)

-- ---- 窗口样式 ----------------------------------------------------------
sky.set_window_style{
    corner_radius  = 16,
    title_color    = 0x232A45,
    border_color   = 0x7A88C8,
    separator_color= 0x39406B,
    paper_color    = 0x05060A,
    ink_color      = 0xFFFFFF,
    close_color    = 0xFFCED8,
    shadow_alpha   = 55,
}

local st = sky.get_style()
sky.log(string.format("desktop.lua applied: radius=%d", st.corner_radius))
