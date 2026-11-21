//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* lapic_tier0.cpp —— LAPIC 调度/IPI 入口的 Tier0 跳板。 */
#include <arch/x86_64/lapic/lapic.h>

namespace {
void (*g_eoi)(void) = nullptr;
void (*g_stop)(void) = nullptr;
void (*g_oneshot)(uint32_t, uint64_t) = nullptr;
void (*g_ipi)(uint32_t, uint32_t) = nullptr;
void (*g_others)(uint32_t, uint32_t) = nullptr;
void (*g_init)(void) = nullptr;
uint64_t (*g_init_timer)(void) = nullptr;
}

extern "C" void lapic_set_hooks(void (*eoi)(void), void (*stop)(void),
                                void (*oneshot)(uint32_t, uint64_t),
                                void (*ipi)(uint32_t, uint32_t),
                                void (*others)(uint32_t, uint32_t),
                                void (*init)(void),
                                uint64_t (*init_timer)(void))
{
    __atomic_store_n(&g_eoi, eoi, __ATOMIC_RELEASE);
    __atomic_store_n(&g_stop, stop, __ATOMIC_RELEASE);
    __atomic_store_n(&g_oneshot, oneshot, __ATOMIC_RELEASE);
    __atomic_store_n(&g_ipi, ipi, __ATOMIC_RELEASE);
    __atomic_store_n(&g_others, others, __ATOMIC_RELEASE);
    __atomic_store_n(&g_init, init, __ATOMIC_RELEASE);
    __atomic_store_n(&g_init_timer, init_timer, __ATOMIC_RELEASE);
}

namespace LAPIC {
void Init() { auto fn = __atomic_load_n(&g_init, __ATOMIC_ACQUIRE); if (fn) fn(); }
uint64_t InitTimer() { auto fn = __atomic_load_n(&g_init_timer, __ATOMIC_ACQUIRE); return fn ? fn() : 0; }
void EOI() { auto fn = __atomic_load_n(&g_eoi, __ATOMIC_ACQUIRE); if (fn) fn(); }
void StopTimer() { auto fn = __atomic_load_n(&g_stop, __ATOMIC_ACQUIRE); if (fn) fn(); }
void Oneshot(uint32_t v, uint64_t ms) { auto fn = __atomic_load_n(&g_oneshot, __ATOMIC_ACQUIRE); if (fn) fn(v, ms); }
void IPI(uint32_t id, uint32_t d) { auto fn = __atomic_load_n(&g_ipi, __ATOMIC_ACQUIRE); if (fn) fn(id, d); }
void IPIOthers(uint32_t id, uint32_t v) { auto fn = __atomic_load_n(&g_others, __ATOMIC_ACQUIRE); if (fn) fn(id, v); }
}
