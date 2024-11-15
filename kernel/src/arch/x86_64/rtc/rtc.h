#pragma once

#include "../pit/pit.h"

namespace RTC
{
    void InitRTC();

    extern int Second, Minute, Hour, Day, Month, Year;
    extern unsigned long LastUpdateTime;

    void read_rtc();
    void UpdateTimeIfNeeded();

}