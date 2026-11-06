//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
/* ============================================================================
 * arch_acpi.cpp —— 内建组件 kernel.arch.acpi (Tier1 POLICY)
 * ----------------------------------------------------------------------------
 * 迁移自 x86_64_init()：
 *   InitFunc("ACPI", ACPI::Init((void*)RSDP_ADDR));   // 原 init.cpp line 162
 *   InitFunc("MADT", MADT_Init());                    // 原 init.cpp line 163
 *
 * 导出私有契约 kernel.acpi.v1（见 kernel/include/kmod/contracts/acpi.h）：
 * 供 kernel.arch.apic / kernel.pci.core / 未来平台驱动组件只读查询 ACPI 根表、
 * MADT 与 MCFG。组件不解释 AML、不做电源管理动作（仍属 Tier0/驱动自行处理）。
 *
 * 失败语义：ACPI 根表缺失时 init 仍返回 KMOD_OK（available()==0），
 * 后续组件按 available/mcfg==NULL 走 legacy 分支 —— 与旧 hcf 行为不同，
 * 但旧 init.cpp 里 ACPI 失败也只是打印错误后继续（ACPI::Init 返回 0 而非崩溃），
 * 这里保持"启动不因 ACPI 表缺失而中断"。
 * ==========================================================================*/
#include <kmod/kmod_component.h>
#include <kmod/contracts/acpi.h>

#include <acpi/acpi.h>
#include <acpi/madt.h>
#include <klib/klib.h>   /* RSDP_ADDR */

static int32_t arch_acpi_init(void);
static void arch_acpi_exit(void);

KCOMPONENT_BEGIN(arch_acpi, "kernel.arch.acpi", "1.0.0", KMOD_TIER_POLICY,
                 KMOD_MF_BOOT, (uint64_t)(KMOD_CAP_MMIO | KMOD_CAP_LOG), 0,
                 "ACPI/MADT table discovery (RSDP -> RSDT/XSDT -> MADT/MCFG)",
                 arch_acpi_init, arch_acpi_exit, 0, 0, 5000u, 1000u)

KCOMPONENT_IMPORT_FN(arch_acpi, kprintf_, int, (const char *, ...), "kernel",
                     "kprintf_", "kernel.log.v1")

namespace {

bool g_acpi_ok = false;
void *g_madt_tbl = nullptr;
void *g_mcfg_tbl = nullptr;

int32_t acpi_available(void)
{
    return g_acpi_ok ? 1 : 0;
}

void *acpi_madt(void)
{
    return g_madt_tbl;
}

void *acpi_mcfg(void)
{
    return g_mcfg_tbl;
}

void *acpi_find_table(const char *signature)
{
    if (!g_acpi_ok || !signature)
        return nullptr;
    return ACPI::FindTable(signature);
}

const kmod_acpi_ops_t g_acpi_ops = {
    KMOD_ACPI_ABI_V1,
    (uint32_t)sizeof(kmod_acpi_ops_t),
    acpi_available,
    acpi_madt,
    acpi_mcfg,
    acpi_find_table,
};

} /* namespace */

static int32_t arch_acpi_init(void)
{
    (void)ACPI::Init((void *)RSDP_ADDR);
    MADT_Init();
    g_madt_tbl = ACPI::FindTable("APIC");
    g_mcfg_tbl = (void *)ACPI::mcfg;
    g_acpi_ok = (ACPI::rootThing != nullptr);
    kprintf_("[comp.arch.acpi] init: RSDP=0x%llx root=%p madt=%p mcfg=%p\n",
             (unsigned long long)RSDP_ADDR, (void *)ACPI::rootThing, g_madt_tbl,
             g_mcfg_tbl);
    if (!g_acpi_ok)
        kprintf_("[comp.arch.acpi] WARN: ACPI root table unavailable "
                 "(legacy fallback path)\n");
    return KMOD_OK;
}

static void arch_acpi_exit(void)
{
    g_acpi_ok = false;
    g_madt_tbl = nullptr;
    g_mcfg_tbl = nullptr;
    kprintf_("[comp.arch.acpi] exit (tables owned by Tier0 ACPI subsystem)\n");
}

KCOMPONENT_EXPORT_CONTRACT(arch_acpi, KMOD_CONTRACT_ACPI_V1, g_acpi_ops,
                           KMOD_ACPI_ABI_V1)
KCOMPONENT_END(arch_acpi)
