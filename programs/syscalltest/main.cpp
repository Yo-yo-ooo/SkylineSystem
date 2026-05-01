/*
* SPDX-License-Identifier: GPL-2.0-only
* File: serial.cpp
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
#include "syscall.h"

typedef struct FrameBuffer
{
	void* BaseAddress;
	uint64_t Width;
	uint64_t Height;
	uint64_t PixelsPerScanLine;
}Framebuffer;


int main(){
    const char *msg = "Hello, World!";
    FrameBuffer fb;
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    syscall(25, 6/*FrameBuffer Type*/, 0/*Fb IDX*/, (uint64_t)&fb, 0, 0, 0); // Get Fb Info
    uint64_t FbAddr = syscall(21, 6/*FrameBuffer Type*/, 0/*Fb IDX*/, 0, 0, 0, 0); // Map Fb
    
    // Now we can use fb to draw something on the screen, for example, fill the
    // screen with red color:
    uint32_t *pixels = (uint32_t *)FbAddr;
    fb.BaseAddress = (void*)FbAddr;
    for (uint64_t y = 0; y < fb.Height; y++) {
        for (uint64_t x = 0; x < fb.Width; x++) {
            pixels[y * fb.PixelsPerScanLine + x] = 0xff800000; // ARGB: Red
        }
    }
    syscall(9, 0, 0, 0, 0, 0, 0); // Exit
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    
    while (true);
    return 0;
}