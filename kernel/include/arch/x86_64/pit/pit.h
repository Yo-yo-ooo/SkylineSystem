/*
* SPDX-License-Identifier: GPL-2.0-only
* File: pit.h
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
#include <stdint.h>
#include <klib/kio.h>
#include <arch/x86_64/pit/pit.h>
namespace PIT
{
    extern uint64_t TicksSinceBoot;
    extern uint64_t freq;
    extern uint16_t Divisor;
    static const uint64_t BaseFrequency = 1193182;
    extern bool Inited;
    extern int32_t FreqAdder;
    extern uint16_t NonMusicDiv;

    void Handler(registers *r);

    void Sleepd(uint64_t seconds);
    void Sleep(uint64_t milliseconds);
    void InitPIT();

    void SetDivisor(uint16_t divisor);
    uint64_t TimeSinceBootMS();
    uint64_t TimeSinceBootMicroS();
    uint64_t GetFrequency();
    void SetFrequency(uint64_t frequency);
    void Tick();
}
