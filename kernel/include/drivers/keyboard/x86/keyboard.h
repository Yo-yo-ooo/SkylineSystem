/*
* SPDX-License-Identifier: GPL-2.0-only
* File: keyboard.h
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

#include <klib/klib.h>

#if !defined(__x86_64__)
#error "Panic (/kernel/src/drivers/keyboard/x86): This keyboard driver is only for x86_64!"
#else

#define EV_KEY 0x1

//extern vfs_node *kb_node;

typedef struct
{
    u16 type;  // key mod etc
    u16 value; // pressed released repeated
    u8 code;
} keyboard_event;

void keyboard_init();

#endif