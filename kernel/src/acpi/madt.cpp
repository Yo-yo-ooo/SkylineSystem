/*
* SPDX-License-Identifier: GPL-2.0-only
* File: madt.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <acpi/madt.h>
#include <acpi/acpi.h>

volatile madt_ioapic_t *madt_ioapic = nullptr;
volatile madt_iso_t *madt_iso_list[16] = {0};
volatile uint64_t madt_apic_address = 0;

void MADT_Init() {
    //acpi_madt* madt = (acpi_madt*)ACPI::FindTable(ACPI::rootThing,"APIC",ACPI::ACPI_DIV);

    _memset(madt_iso_list, 0, 16 * sizeof(madt_iso_t));
    madt_t *madt = (madt_t*)ACPI::FindTable(ACPI::rootThing,"APIC",ACPI::ACPI_DIV);
    ASSERT(PHYSICAL(madt));
    kpok("Found MADT.\n");
    madt_apic_address = madt->lapic_address;
    uint64_t offset = 0;
    while (offset < madt->header.Length - sizeof(madt_t)) {
        madt_entry_t *entry = (madt_entry_t*)(madt->table + offset);
        if (entry->type == 1) {
            if (madt_ioapic)
                kwarn("More than 1 I/O APICs found.\n");
            else
                madt_ioapic = (madt_ioapic_t*)entry->data;
        } else if (entry->type == 2) {
            madt_iso_t *iso = (madt_iso_t*)entry->data;
            if (madt_iso_list[iso->irq])
                kwarn("Found ISO for (already found) IRQ #%d.\n", iso->irq);
            else {
                kpok("Found ISO for IRQ %d.\n", iso->irq);
                madt_iso_list[iso->irq] = iso;
            }
        } else if (entry->type == 5) {
            kinfo("Found APIC Address Override.\n");
            madt_lapic_ovr_t *lapic_ovr = (madt_lapic_ovr_t*)entry->data;
            madt_apic_address = lapic_ovr->lapic_addr;
        }
        offset += entry->len;
    }
}