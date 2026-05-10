/*
* SPDX-License-Identifier: GPL-2.0-only
* File: xcr.h
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
#ifndef _x86_64_ASM_XCR_H
#define _x86_64_ASM_XCR_H

#include <stdint.h>
#include <stddef.h>
#include <klib/kio.h>
#include <pdef.h>


constexpr uint8_t XCR_XSTATE_FEATURES_ENABLED = 0;
constexpr uint8_t XCR_XSTATE_FEATURES_IN_USE = 1;


static inline uint64_t read_xcr(const uint8_t xcr) {
    uint32_t eax = 0;
    uint32_t edx = 0;

    asm volatile ("xgetbv" : "=a"(eax), "=d"(edx) : "c"(xcr));
    return (uint64_t)edx << 32 | eax;
}

static inline void write_xcr(const uint8_t xcr, const uint64_t val) {
    asm volatile ("xsetbv"
                  :: "a"((uint32_t)val), "c"(xcr), "d"((uint32_t)(val >> 32))
                  : "memory");
}

#endif