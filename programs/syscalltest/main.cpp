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
#include <syscall.h>
#include <gui/fb.h>
#include <gui/basicdraw.h>

char intTo_stringOutput[128];
const char *to_string(uint64_t value)
{
    uint8_t size = 0;
    uint64_t sizetest = value;
    while (sizetest / 10 > 0)
    {
        sizetest /= 10;
        size++;
    }

    uint8_t index = 0;
    while (value / 10 > 0)
    {
        uint8_t remainder = value % 10;
        value /= 10;
        intTo_stringOutput[size - index] = remainder + '0';
        index++;
    }
    uint8_t remainder = value % 10;
    intTo_stringOutput[size - index] = remainder + '0';
    intTo_stringOutput[size + 1] = 0;
    intTo_stringOutput[size + 2] = 0;
    return intTo_stringOutput;
}

int main(){
    const char *msg = "Hello, World!";
    FrameBuffer fb;
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    uint64_t FbAddr = MapFB();
    fb = GetFBInfo();
    fb.BaseAddress = (void*)FbAddr;
    
    BasicDraw bd(&fb);
     // 绘制自适应UI
    bd.DrawProportionalUI();

    while (true);
    

    syscall(9, 0, 0, 0, 0, 0, 0); // Exit
    syscall(24, (long)msg, 13, 0, 0, 0, 0);
    
    while (true);
    return 0;
}