/*
* SPDX-License-Identifier: GPL-2.0-only
* File: basicdraw.h
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#pragma once
#ifndef _DEAKTOP_BASIC_DRAW_H_
#define _DEAKTOP_BASIC_DRAW_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>
#include <klib/renderer/fb.h>

#ifdef CONFIG_USE_DESKTOP_SUBSYS

class BasicDraw
{
private:
    Framebuffer *FrameBuf;
public:
    BasicDraw(Framebuffer *fb) : FrameBuf(fb) {}

    void ClearScreen(uint32_t Color);
    void PutPixel(uint64_t X,uint64_t Y,uint32_t Color);

    void DrawHLine(uint64_t X, uint64_t Y, uint64_t Width, uint32_t Color); // 画水平线（极快）
    void DrawVLine(uint64_t X, uint64_t Y, uint64_t Height, uint32_t Color); // 画垂直线
    void DrawRect(uint64_t X, uint64_t Y, uint64_t Width, uint64_t Height, uint32_t Color, bool Fill);
    void DrawLine(int64_t x0, int64_t y0, int64_t x1, int64_t y1, uint32_t Color); // 万能画线
    void DrawWindow(uint64_t X, uint64_t Y, uint64_t W, uint64_t H, const char* Title);
    void DrawCircle(uint64_t xc, uint64_t yc, uint64_t r, uint32_t Color);

    ~BasicDraw(){}
};

#endif

#endif