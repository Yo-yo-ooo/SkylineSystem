//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <arch/x86_64/pit/pit.h>

namespace RTC
{
    void InitRTC();

    extern int32_t Second, Minute, Hour, Day, Month, Year;
    extern unsigned long LastUpdateTime;

    void read_rtc();
    void UpdateTimeIfNeeded();
    uint32_t ToUnixTime();

}

static int64_t RTC_INIT_YEAR;
static int64_t RTC_INIT_MONTH;
static int64_t RTC_INIT_DAY;
static int64_t RTC_INIT_HOUR;
static int64_t RTC_INIT_MINUTE;
static int64_t RTC_INIT_SECOND;
static int64_t TIME_SINCE_RTC_INITED_SECOND;