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