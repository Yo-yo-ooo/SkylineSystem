/*
* SPDX-License-Identifier: GPL-2.0-only
* File: ioapic.h
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
#pragma once

#include <klib/klib.h>
#include <acpi/madt.h>
#include <acpi/acpi.h>

#define IOAPIC_REGSEL 0x0
#define IOAPIC_IOWIN  0x10

#define IOAPIC_ID     0x0
#define IOAPIC_VER    0x01
#define IOAPIC_ARB    0x02
#define IOAPIC_REDTBL 0x10



namespace IOAPIC{
    extern volatile uint64_t ioapic_address;

    void Init();

    uint32_t Read(uint8_t reg);

    void Write(uint8_t reg, uint32_t data);

    void RemapGSI(uint32_t lapic_id, uint32_t gsi, uint8_t vec, uint32_t flags);
    void RemapIRQ(uint32_t lapic_id, uint8_t irq, uint8_t vec, bool masked);
}