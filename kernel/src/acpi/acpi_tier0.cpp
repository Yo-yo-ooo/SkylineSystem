//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* acpi_tier0.cpp —— ACPI 全局表指针 + FindTable 跳板。
 *
 * acpi.cpp 的解析实现已迁入 kernel.arch.acpi.ko；madt/lapic/ioapic/pci 仍
 * 直接读这些 Tier0 全局量，或通过这里的函数指针查表。 */
#include <acpi/acpi.h>
#include <acpi/madt.h>

namespace ACPI {
MCFGHeader *mcfg = nullptr;
FADTHeader *FADT = nullptr;
int32_t ACPI_DIV = 0;
SDTHeader *rootThing = nullptr;
volatile bool UseXSDT = false;
}

namespace {
void *(*g_find1)(const char *) = nullptr;
void *(*g_find3)(ACPI::SDTHeader *, const char *, int32_t) = nullptr;
}

extern "C" void acpi_set_find_table(void *(*find1)(const char *),
                                    void *(*find3)(ACPI::SDTHeader *, const char *, int32_t))
{
    __atomic_store_n(&g_find1, find1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_find3, find3, __ATOMIC_RELEASE);
}

namespace ACPI {
void *FindTable(const char *signature)
{
    auto fn = __atomic_load_n(&g_find1, __ATOMIC_ACQUIRE);
    return fn ? fn(signature) : nullptr;
}

void *FindTable(SDTHeader *sdtHeader, const char *signature, int32_t div)
{
    auto fn = __atomic_load_n(&g_find3, __ATOMIC_ACQUIRE);
    return fn ? fn(sdtHeader, signature, div) : nullptr;
}
}

/* MADT 全局量迁到 Tier0：lapic/ioapic 内核代码直接读。 */
madt_ioapic_t *madt_ioapic = nullptr;
madt_iso_t *madt_iso_list[16] = {0};
uint64_t madt_apic_address = 0;
