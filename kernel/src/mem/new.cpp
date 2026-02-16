/*
* SPDX-License-Identifier: GPL-2.0-only
* File: new.cpp
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
//#include <mem/heap.h>
#include <stddef.h>
#include <mem/new2.h>
const char* _X__file__ = "unknown";
const char* _X__func__ = "unknown";
size_t _X__line__ = 0;

void* operator new(size_t size) {
    void *ptr = _Ymalloc(size, _X__func__, _X__file__, _X__line__);
    _X__file__ = "unknown";
    _X__func__ = "unknown";
    _X__line__ = 0;
    return ptr;
}