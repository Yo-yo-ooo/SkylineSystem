/*
* SPDX-License-Identifier: GPL-2.0-only
* File: hpet.h
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
#ifndef _X86_64_HPET_H_
#define _X86_64_HPET_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <acpi/acpi.h>
#include <conf.h>

typedef struct [[gnu::packed]] {
    ACPI::SDTHeader hdr;

    uint8_t hardware_rev_id;
    uint8_t comparator_count:5;
    uint8_t counter_size:1;
    uint8_t reserved:1;
    uint8_t legacy_replace:1;
    uint16_t pci_vendor_id;

    ACPI::gas_t base_addr;

    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} hpet_sdt_t;

typedef struct[[gnu::packed]] {
    volatile uint64_t config_and_capabilities;
    volatile uint64_t comparator_value;
    volatile uint64_t fsb_interrupt_route;
    volatile uint64_t unused;
} hpet_timer_t;

typedef struct[[gnu::packed]] {
    volatile uint64_t general_capabilities;
    volatile uint64_t unused0;
    volatile uint64_t general_configuration;
    volatile uint64_t unused1;
    volatile uint64_t general_int_status;
    volatile uint64_t unused2;
    volatile uint64_t unused3[2][12];
    volatile uint64_t main_counter_value;
    volatile uint64_t unused4;
    hpet_timer_t timers[];
} hpet_t;

namespace HPET
{
    
    void InitHPET();
    uint64_t GetTimeNS();
    void SleepNS(uint64_t ns);
}

#endif