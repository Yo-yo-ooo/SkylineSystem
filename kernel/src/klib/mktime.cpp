//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* 公历 UTC 秒计算：PIT/RTC 初始化与 syscall.time 共用。
 * 原实现位于 modules/builtin/x86_64/syscall/time.cpp；该文件迁入 .ko 后，
 * 内核侧 PIT 不能依赖模块符号，故收进 Tier0 klib。 */
#include <stdint.h>

uint64_t mktime(uint32_t year, uint32_t mon,
                uint32_t day, uint32_t hour,
                uint32_t min, uint32_t sec)
{
    if (0 >= (int)(mon -= 2)) {    /* 1..12 -> 11,12,1..10 */
        mon += 12;                  /* puts Feb last since it has leap day */
        year -= 1;
    }
    return (((
             (uint64_t)(year / 4 - year / 100 + year / 400 + 367 * mon / 12 + day) +
             year * 365 - 719499
            ) * 24 + hour) * 60 + min) * 60 + sec;
}
