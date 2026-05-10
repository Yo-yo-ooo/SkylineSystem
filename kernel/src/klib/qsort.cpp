/*
* SPDX-License-Identifier: GPL-2.0-only
* File: qsort.cpp
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

void qsort(void *base, size_t num, size_t width, int32_t (*sort)(const void *e1, const void *e2)) {
    for (int32_t i = 0; i < (int32_t)num - 1; i++) {
        for (int32_t j = 0; j < (int32_t)num - 1 - i; j++) {
            if (sort((char *)base + j * width, (char *)base + (j + 1) * width) > 0) {
                for (int32_t i = 0; i < (int32_t)width; i++) {
                    char temp                           = ((char *)base + j * width)[i];
                    ((char *)base + j * width)[i]       = ((char *)base + (j + 1) * width)[i];
                    ((char *)base + (j + 1) * width)[i] = temp;
                }
            }
        }
    }
}