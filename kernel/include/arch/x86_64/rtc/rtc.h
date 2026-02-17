/*
* SPDX-License-Identifier: GPL-2.0-only
* File: rtc.h
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

#include <arch/x86_64/pit/pit.h>

namespace RTC
{
    void InitRTC();

    extern int32_t Second, Minute, Hour, Day, Month, Year;
    extern unsigned long LastUpdateTime;

    void read_rtc();
    void UpdateTimeIfNeeded();

}

static int64_t RTC_INIT_YEAR;
static int64_t RTC_INIT_MONTH;
static int64_t RTC_INIT_DAY;
static int64_t RTC_INIT_HOUR;
static int64_t RTC_INIT_MINUTE;
static int64_t RTC_INIT_SECOND;
static int64_t TIME_SINCE_RTC_INITED_SECOND;