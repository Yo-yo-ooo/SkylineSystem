/*
* SPDX-License-Identifier: GPL-2.0-only
* File: lapic.cpp
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
#include <klib/kio.h>
#include <conf.h>


static volatile uint64_t lapic_address = 0;


namespace LAPIC{

    uint64_t apic_ticks = 0;
    bool x2apic = false;


    void Init() {
        uint64_t apic_msr = rdmsr(0x1B);
        lapic_address = HIGHER_HALF(madt_apic_address);
        apic_msr |= 0x800; // Enable flag
        uint32_t a = 1, b = 0, c = 0, d = 0;
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(a));
        if (c & (1 << 21)) {
            apic_msr |= 0x400; // Enable x2apic
            x2apic = true;
        }
        wrmsr(0x1B, apic_msr);
        LAPIC::Write(0xF0, LAPIC::Read(0xF0) | 0x100); // Spurious interrupt vector
    }

    void Write(uint32_t reg, uint64_t val) {
        if (x2apic) {
            wrmsr((reg >> 4) + 0x800, val);
            return;
        }
        uint64_t addr = lapic_address + reg;
        *((volatile uint32_t*)addr) = val;
        return;
    }

    uint64_t Read(uint32_t reg) {
        if (x2apic) {
            reg = (reg >> 4) + 0x800;
            return rdmsr(reg);
        }
        uint64_t addr = lapic_address + reg;
        return *((volatile uint32_t*)addr);
    }


    uint32_t GetID() {
        if(!x2apic) return LAPIC::Read(0x0020) >> LAPIC_ICDESTSHIFT;
        else return LAPIC::Read(0x0020);
    }

    void EOI() {
        LAPIC::Write(0xb0, 0x0);
    }

    void StopTimer() {
        // We do this to avoid overlapping oneshots
        LAPIC::Write(LAPIC_TIMER_INITCNT, 0);
        LAPIC::Write(LAPIC_TIMER_LVT, LAPIC_TIMER_DISABLE);
    }

    void Oneshot(uint8_t vec, uint64_t ms) {
        LAPIC::Write(LAPIC_TIMER_LVT, LAPIC_TIMER_DISABLE);
        LAPIC::Write(LAPIC_TIMER_INITCNT, 0);
        LAPIC::Write(LAPIC_TIMER_DIV,0x3);
        LAPIC::Write(LAPIC_TIMER_INITCNT, this_cpu()->lapic_ticks * ms);
        LAPIC::Write(LAPIC_TIMER_LVT, vec);
    }

    uint64_t InitTimer() {
        LAPIC::Write(LAPIC_TIMER_DIV, 0x3);
        LAPIC::Write(LAPIC_TIMER_INITCNT, 0xFFFFFFFF);
        PIT::Sleep(1); // 1 ms
        LAPIC::Write(LAPIC_TIMER_LVT, LAPIC_TIMER_DISABLE);
        uint32_t ticks = 0xFFFFFFFF - LAPIC::Read(LAPIC_TIMER_CURCNT);
        return ticks;
    }

    void IPI(uint32_t id, uint32_t dat) {
        if (x2apic) {
            LAPIC::Write(0x300, ((uint64_t)id << 32) | dat);
            return;
        }
        LAPIC::Write(LAPIC_ICRHI, id << LAPIC_ICDESTSHIFT);
        LAPIC::Write(LAPIC_ICRLO, dat);
    }

    void IPIAll(uint32_t lapic_id, uint32_t vector) {
        LAPIC::IPI(lapic_id, vector | 0x80000);
    }

    void IPIOthers(uint32_t lapic_id, uint32_t vector) {
        LAPIC::IPI(lapic_id, vector | 0xC0000);
    }

}