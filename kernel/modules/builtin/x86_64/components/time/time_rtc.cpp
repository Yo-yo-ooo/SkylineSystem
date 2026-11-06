//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * time_rtc.cpp —— 内建组件 kernel.time.rtc (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：InitFunc("RTC", RTC::InitRTC());（原 line 205）
 * 旧顺序在 SMP/RemapIRQ 之后（RTC 周期中断走 IRQ8 -> IOAPIC 路由），因此
 * 依赖 kernel.arch.apic。
 *
 * 导出契约 kernel.rtc.v1（contracts/time.h）：to_unix_time() + read()。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/time.h>

#include <arch/x86_64/rtc/rtc.h>

static int32_t time_rtc_init(void);
static void time_rtc_exit(void);

KCOMPONENT_BEGIN(time_rtc, "kernel.time.rtc", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_IRQ | KMOD_CAP_LOG), 0,
                 "CMOS RTC init; exports unix-time read + refresh",
                 time_rtc_init, time_rtc_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(time_rtc, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(time_rtc, "kernel.arch.apic", apic)

namespace {

uint32_t rtc_to_unix_time(void)
{
    return RTC::ToUnixTime();
}

void rtc_read(void)
{
    RTC::read_rtc();
}

const kmod_rtc_ops_t g_rtc_ops = {
    KMOD_RTC_ABI_V1,
    (uint32_t)sizeof(kmod_rtc_ops_t),
    rtc_to_unix_time,
    rtc_read,
};

} /* namespace */

static int32_t time_rtc_init(void)
{
    RTC::InitRTC();
    kprintf_("[comp.time.rtc] init: unix=%u\n", (unsigned)RTC::ToUnixTime());
    return KMOD_OK;
}

static void time_rtc_exit(void)
{
    kprintf_("[comp.time.rtc] exit (RTC periodic interrupt left as Tier0 set)\n");
}

KCOMPONENT_EXPORT_CONTRACT(time_rtc, KMOD_CONTRACT_RTC_V1, g_rtc_ops,
                           KMOD_RTC_ABI_V1)
KCOMPONENT_END(time_rtc)
