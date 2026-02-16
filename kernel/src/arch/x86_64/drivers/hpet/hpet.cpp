/*
* SPDX-License-Identifier: GPL-2.0-only
* File: hpet.cpp
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
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <klib/algorithm/queue.h>
#include <arch/x86_64/drivers/hpet/hpet.h>
#include <arch/x86_64/rtc/rtc.h>
#include <arch/x86_64/pit/pit.h>

static uint64_t hpet_period = 0;
static hpet_t *hpet = nullptr;

namespace HPET
{
    void InitHPET(){
        hpet_sdt_t *hpet_sdt = (hpet_sdt_t *) ACPI::FindTable("HPET");
        if (hpet_sdt == nullptr) {
            kerror("HPET: HPET ACPI table not found!\n");
            return;
        }
        VMM::Map(kernel_pagemap,VIRTUAL(hpet_sdt->base_addr.address),
                 hpet_sdt->base_addr.address,
                 VMM_FLAGS_MMIO);
        hpet = (hpet_t *) VIRTUAL(hpet_sdt->base_addr.address);
        uint64_t tmp = hpet->general_capabilities;

        /* Check that the HPET is valid or not */
        if (!(tmp & (1 << 15))) {
            kerror("HPET is not legacy replacement capable\n");
            hpet = NULL;
            return;
        }

        /* Calculate HPET frequency (f = 10^15 / period) */
        uint64_t counter_clk_period = tmp >> 32;
        uint64_t frequency = 1000000000000000 / counter_clk_period;

        kinfo("HPET: Detected frequency of %d Hz\n", frequency);
        hpet_period = counter_clk_period / 1000000;

        /* Set ENABLE_CNF bit */
        hpet->general_configuration = hpet->general_configuration | 0x1;

        kinfo("HPET initialization finished\n");
    }
    uint64_t GetTimeNS(){
        if(hpet == nullptr)
            return PIT::TicksSinceBoot;
        uint64_t tf = hpet->main_counter_value * hpet_period;
        return tf;
    }
    void SleepNS(uint64_t ns){
        uint64_t stt = HPET::GetTimeNS();
        uint64_t tgt = HPET::GetTimeNS() + ns;
        while (true) {
            uint64_t cur = HPET::GetTimeNS();
            if (cur >= tgt)
                break;
            if (cur <= stt) {
                break;
            }
            asm volatile ("nop");
        }
    }
}