/*
* SPDX-License-Identifier: GPL-2.0-only
* File: mair.h
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
#ifndef ARCH_AARCH64_ASM_MAIR_H
#define ARCH_AARCH64_ASM_MAIR_H

#include <stdint.h>

static inline uint64_t read_mair_el1() {
    uint64_t value = 0;
    asm volatile ("mrs %0, mair_el1" : "=r"(value));

    return value;
}

static inline void write_mair_el1(const uint64_t value) {
    asm volatile ("msr mair_el1, %0" :: "r"(value));
}

#endif