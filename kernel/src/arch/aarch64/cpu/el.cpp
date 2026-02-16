/*
* SPDX-License-Identifier: GPL-2.0-only
* File: el.cpp
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
#include <arch/aarch64/cpu/features.h>

uint8_t GetCurrentEL(){
    uint64_t result = 0;
    asm volatile ("mrs %0, CurrentEL " : "=r"(result));

    switch (result){
    case 0xC:
        return 3;
    case 0x8:
        return 2;
    case 0x4:
        return 1;
    case 0x0:
        return 0;
    default:
        return -1;
    }
    return -1;
}