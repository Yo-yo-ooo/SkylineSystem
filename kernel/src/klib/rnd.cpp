/*
* SPDX-License-Identifier: GPL-2.0-only
* File: rnd.cpp
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
#include <klib/rnd.h>

#ifdef __x86_64__
#pragma GCC target("sse2")

namespace RND
{
    __uint128_t g_lehmer64_state = 1337;

    uint64_t lehmer64() 
    {
        g_lehmer64_state *= 0xda942042e4dd58b5;
        return g_lehmer64_state >> 64;
    }
    // https://www.codingame.com/playgrounds/58096/how-to-randomize-bits-and-integers-efficiently-in-c

    double convert_uint64_to_double(uint64_t value)
    {
        uint64_t u64 = 0x3FF0000000000000ULL | ((value >> 12) | 1) ;
        return *(double*)&u64 - 1.0;
    }
    // https://stackoverflow.com/questions/52147419/how-to-convert-random-uint64-t-to-random-double-in-range-0-1-using-bit-wise-o

    double RandomDouble()
    {
        int amt = lehmer64() & 0x0F;
        for (int i = 0; i < amt; ++i)
            lehmer64();
        
        return convert_uint64_to_double(lehmer64());
    }

    uint64_t RandomInt()
    {
        int amt = (lehmer64() & 0x0F) + 7;
        for (int i = 0; i < amt; ++i)
            lehmer64();

        return lehmer64();
    }
};
#pragma GCC pop_options
#endif