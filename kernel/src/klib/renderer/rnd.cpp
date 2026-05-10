/*
* SPDX-License-Identifier: GPL-2.0-only
* File: rnd.cpp
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
#include <klib/renderer/rnd.h>
#include <klib/renderer/fb.h>
#include <flanterm/backends/fb.h>
#include <flanterm/flanterm.h>

extern struct flanterm_context* ft_ctx;

namespace Renderer
{
    void Clear(uint32_t col)
    {
        uint64_t fbBase = (uint64_t)Fb->BaseAddress;
        uint64_t pxlsPerScanline = Fb->PixelsPerScanLine;
        uint64_t fbHeight = Fb->Height;

        for (int64_t y = 0; y < Fb->Height; y++)
            for (int64_t x = 0; x < Fb->Width; x++)
                *((uint32_t*)(fbBase + 4 * (x + pxlsPerScanline * y))) = col;

        //ft_ctx->clear(ft_ctx, true);       
    }
}