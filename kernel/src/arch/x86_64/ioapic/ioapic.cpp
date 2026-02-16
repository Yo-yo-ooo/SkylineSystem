/*
* SPDX-License-Identifier: GPL-2.0-only
* File: ioapic.cpp
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
#include <arch/x86_64/allin.h>
#include <arch/x86_64/ioapic/ioapic.h>

#define REDTBL(n) (0x10 + 2 * n)


#define IOAPIC_REGSEL 0x0
#define IOAPIC_IOWIN  0x10

#define IOAPIC_ID     0x0
#define IOAPIC_VER    0x01
#define IOAPIC_ARB    0x02

namespace IOAPIC{
    volatile uint64_t ioapic_address = NULL;

    void Init(){
        ioapic_address = HIGHER_HALF((uint64_t)madt_ioapic->ioapic_addr);
        ASSERT(PHYSICAL(ioapic_address));
        kinfoln("HIT!");
        uint32_t value = IOAPIC::Read(IOAPIC_VER);
        uint32_t count = ((value >> 16) & 0xFF) + 1;
        kinfoln("HIT!2");
        kinfoln("IOAPIC COUNT:%d",count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t low = IOAPIC::Read(REDTBL(i));
            IOAPIC::Write(REDTBL(i),low | (1 << 16));
        }
        //for(;;){kinfoln("HIT 3");}
    }
    void Write(uint8_t reg, uint32_t data) {
        *(volatile uint32_t*)ioapic_address = reg;
        *(volatile uint32_t*)(ioapic_address + 0x10) = data;
    }

    uint32_t Read(uint8_t reg) {
        *(volatile uint32_t*)ioapic_address = reg;
        return *(volatile uint32_t*)(ioapic_address + 0x10);
    }

    void RemapGSI(uint32_t lapic_id, uint32_t gsi, uint8_t vec, uint32_t flags) {
        uint64_t value = vec;
        value |= flags;
        value |= (uint64_t)lapic_id << 56;
        IOAPIC::Write(REDTBL(gsi), (uint32_t)value);
        IOAPIC::Write(REDTBL(gsi)+1, (uint32_t)(value >> 32));
    }

    void RemapIRQ(uint32_t lapic_id, uint8_t irq, uint8_t vec, bool masked) {
        madt_iso_t *iso = madt_iso_list[irq];
        if (!iso) {
            IOAPIC::RemapGSI(lapic_id, irq, vec, (masked ? 1 << 16 : 0));
            return;
        }
        uint32_t flags = 0;
        if (iso->flags & (1 << 1)) flags |= 1 << 13;
        if (iso->flags & (1 << 3)) flags |= 1 << 15;
        if (masked) flags |= (1 << 16);
        IOAPIC::RemapGSI(lapic_id, iso->gsi, vec, flags);
    }
}