//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * time_hpet.cpp —— 内建组件 kernel.time.hpet (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：InitFunc("HPET", HPET::InitHPET());（原 line 176）
 *
 * 依赖 kernel.arch.acpi：HPET::InitHPET() 通过 ACPI::FindTable("HPET") 查表；
 * 任务书建议依赖 core，但实现上必须有 ACPI 表解析在前，否则必然走
 * "HPET ACPI table not found" 分支 —— 因此显式依赖 acpi 组件（报告里已注明）。
 *
 * 导出契约 kernel.hpet.v1（contracts/time.h）：
 *   get_time_ns() / available()（不可用时旧实现会退化为 tick 计数，调用方
 *   必须先查 available）。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/time.h>

#include <arch/x86_64/drivers/hpet/hpet.h>

static int32_t time_hpet_init(void);
static void time_hpet_exit(void);

KCOMPONENT_BEGIN(time_hpet, "kernel.time.hpet", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_MMIO | KMOD_CAP_LOG), 0,
                 "HPET init (ACPI HPET table); exports ns counter + availability",
                 time_hpet_init, time_hpet_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(time_hpet, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")
KCOMPONENT_DEPENDS(time_hpet, "kernel.arch.acpi", acpi)

namespace {

uint64_t hpet_time_ns(void)
{
    return HPET::GetTimeNS();
}

int32_t hpet_available(void)
{
    return HPET::Available() ? 1 : 0;
}

const kmod_hpet_ops_t g_hpet_ops = {
    KMOD_HPET_ABI_V1,
    (uint32_t)sizeof(kmod_hpet_ops_t),
    hpet_time_ns,
    hpet_available,
};

} /* namespace */

static int32_t time_hpet_init(void)
{
    HPET::InitHPET();
    kprintf_("[comp.time.hpet] init: available=%d\n",
             (int)(HPET::Available() ? 1 : 0));
    return KMOD_OK;
}

static void time_hpet_exit(void)
{
    kprintf_("[comp.time.hpet] exit (HPET counter left running)\n");
}

KCOMPONENT_EXPORT_CONTRACT(time_hpet, KMOD_CONTRACT_HPET_V1, g_hpet_ops,
                           KMOD_HPET_ABI_V1)
KCOMPONENT_END(time_hpet)
