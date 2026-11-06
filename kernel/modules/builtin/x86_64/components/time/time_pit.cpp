//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * time_pit.cpp —— 内建组件 kernel.time.pit (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：InitFunc("PIT & RTC", PIT::InitPIT());（原 line 175）
 * 依赖 kernel.arch.apic：PIT IRQ0 需要 IOAPIC 路由（apic 组件已把 IRQ0
 * remap 到 vector 32）。
 *
 * 导出契约 kernel.time.v1（见 kernel/include/kmod/contracts/time.h）：
 *   monotonic_ms() = PIT::MonotonicMS()（校准时走 TSC，不受 IF 影响）
 *   ticks()        = PIT::TicksSinceBoot（IRQ0 累计节拍）
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/time.h>

#include <arch/x86_64/pit/pit.h>

static int32_t time_pit_init(void);
static void time_pit_exit(void);

KCOMPONENT_BEGIN(time_pit, "kernel.time.pit", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_IRQ | KMOD_CAP_LOG), 0,
                 "PIT channel0 init; exports monotonic ms + IRQ0 tick counter",
                 time_pit_init, time_pit_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(time_pit, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(time_pit, "kernel.arch.apic", apic)

namespace {

uint64_t pit_monotonic_ms(void)
{
    return PIT::MonotonicMS();
}

uint64_t pit_ticks(void)
{
    return PIT::TicksSinceBoot;
}

const kmod_time_ops_t g_time_ops = {
    KMOD_TIME_ABI_V1,
    (uint32_t)sizeof(kmod_time_ops_t),
    pit_monotonic_ms,
    pit_ticks,
};

} /* namespace */

static int32_t time_pit_init(void)
{
    PIT::InitPIT();
    kprintf_("[comp.time.pit] init: PIT programmed (freq=%llu Hz)\n",
             (unsigned long long)PIT::freq);
    return KMOD_OK;
}

static void time_pit_exit(void)
{
    kprintf_("[comp.time.pit] exit (PIT left programmed by Tier0)\n");
}

KCOMPONENT_EXPORT_CONTRACT(time_pit, KMOD_CONTRACT_TIME_V1, g_time_ops,
                           KMOD_TIME_ABI_V1)
KCOMPONENT_END(time_pit)
