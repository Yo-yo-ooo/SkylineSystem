/*
* SPDX-License-Identifier: GPL-2.0-only
* File: pmm.h
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

#include <klib/types.h>

#define BuddyMaxOrder	15

namespace PMM{

    extern volatile uint8_t *bitmap;
    extern volatile uint64_t bitmap_size;
    extern volatile uint64_t bitmap_last_free;

    extern volatile uint64_t pmm_bitmap_start;
    extern volatile uint64_t pmm_bitmap_size;
    extern volatile uint64_t pmm_bitmap_pages;
    void bitmap_clear_(uint64_t bit);
    void bitmap_set_(uint64_t bit);
    bool bitmap_test_(uint64_t bit);

    void Init();
    
    void *Request();
    void* Request(uint64_t n);
    void Free(void *ptr);
}