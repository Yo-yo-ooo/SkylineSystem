//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* hpet_tier0.cpp —— HPET 的 Tier0 跳板。hpet.cpp 已迁入 kernel.time.hpet.ko。 */
#include <arch/x86_64/drivers/hpet/hpet.h>

namespace {
bool (*g_avail)(void) = nullptr;
uint64_t (*g_time)(void) = nullptr;
void (*g_sleep)(uint64_t) = nullptr;
}

extern "C" void hpet_set_hooks(bool (*avail)(void), uint64_t (*time_ns)(void), void (*sleep_ns)(uint64_t))
{
    __atomic_store_n(&g_avail, avail, __ATOMIC_RELEASE);
    __atomic_store_n(&g_time, time_ns, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sleep, sleep_ns, __ATOMIC_RELEASE);
}

namespace HPET {
bool Available() { auto fn = __atomic_load_n(&g_avail, __ATOMIC_ACQUIRE); return fn ? fn() : false; }
uint64_t GetTimeNS() { auto fn = __atomic_load_n(&g_time, __ATOMIC_ACQUIRE); return fn ? fn() : 0; }
void SleepNS(uint64_t ns) { auto fn = __atomic_load_n(&g_sleep, __ATOMIC_ACQUIRE); if (fn) fn(ns); }
}
