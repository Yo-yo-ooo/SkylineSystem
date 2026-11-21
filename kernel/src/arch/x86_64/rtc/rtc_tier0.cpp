//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* rtc_tier0.cpp —— RTC 共享字段 + Init/read/update 跳板。
 * rtc.cpp 已迁入 kernel.time.rtc.ko；PIT 与 syscall.time 通过稳定符号访问。 */
#include <arch/x86_64/rtc/rtc.h>

namespace RTC {
int32_t Second = 0, Minute = 0, Hour = 0, Day = 1, Month = 1, Year = 0;
unsigned long LastUpdateTime = 0;
}

namespace {
void (*g_init)(void) = nullptr;
void (*g_read)(void) = nullptr;
void (*g_update)(void) = nullptr;
}

extern "C" void rtc_set_hooks(void (*init_fn)(void), void (*read_fn)(void), void (*update_fn)(void))
{
    __atomic_store_n(&g_init, init_fn, __ATOMIC_RELEASE);
    __atomic_store_n(&g_read, read_fn, __ATOMIC_RELEASE);
    __atomic_store_n(&g_update, update_fn, __ATOMIC_RELEASE);
}

namespace RTC {
void InitRTC() { auto fn = __atomic_load_n(&g_init, __ATOMIC_ACQUIRE); if (fn) fn(); }
void read_rtc() { auto fn = __atomic_load_n(&g_read, __ATOMIC_ACQUIRE); if (fn) fn(); }
void UpdateTimeIfNeeded() { auto fn = __atomic_load_n(&g_update, __ATOMIC_ACQUIRE); if (fn) fn(); }
}
