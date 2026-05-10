/*
* SPDX-License-Identifier: GPL-2.0-only
* File: fb.cpp
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
#include <gui/fb.h>
#include <syscall.h>

FrameBuffer GetFBInfo() {
    FrameBuffer fb;
    fb.BaseAddress = (void*)syscall(26,6,0,0,0,0,0);
    fb.BufferSize = syscall(26,6,0,1,0,0,0);
    fb.Height = syscall(26,6,0,2,0,0,0);
    fb.PixelsPerScanLine = syscall(26,6,0,3,0,0,0);
    fb.Width = syscall(26,6,0,4,0,0,0);
    return fb;
}

uint64_t MapFB() {
    return syscall(21,6,0,0,0,0,0);
}