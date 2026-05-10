/*
* SPDX-License-Identifier: GPL-2.0-only
* File: gdt.cpp
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
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/lapic/lapic.h>

gdt_table_t gdt_tables[MAX_CPU];
gdt_desc_t gdt_desc[MAX_CPU];
tss_desc_t tss_desc[MAX_CPU];

extern "C" void gdt_reload_seg();

namespace GDT{
    void Init(uint32_t cpu_num) {
        tss_desc[cpu_num].iopb = sizeof(tss_desc_t);
        gdt_tables[cpu_num].entries[0] = 0x0000000000000000;
        gdt_tables[cpu_num].entries[1] = 0x00af9b000000ffff; // 0x08 64 bit cs (code)
        gdt_tables[cpu_num].entries[2] = 0x00af93000000ffff; // 0x10 64 bit ss (data)
        gdt_tables[cpu_num].entries[3] = 0x00aff3000000ffff; // 0x18 user mode ss (data)
        gdt_tables[cpu_num].entries[4] = 0x00affb000000ffff; // 0x20 user mode cs (code)

        uint64_t tss = (uint64_t)&tss_desc[cpu_num];

        gdt_tables[cpu_num].tss_entry.len = sizeof(tss_desc_t) - 1;
        gdt_tables[cpu_num].tss_entry.base = (uint16_t)(tss & 0xffff);
        gdt_tables[cpu_num].tss_entry.base1 = (uint8_t)((tss >> 16) & 0xff);
        gdt_tables[cpu_num].tss_entry.flags = 0x89;
        gdt_tables[cpu_num].tss_entry.flags1 = 0;
        gdt_tables[cpu_num].tss_entry.base2 = (uint8_t)((tss >> 24) & 0xff);
        gdt_tables[cpu_num].tss_entry.base3 = (uint32_t)(tss >> 32);
        gdt_tables[cpu_num].tss_entry.resv = 0;

        gdt_desc[cpu_num].size = sizeof(gdt_table_t) - 1;
        gdt_desc[cpu_num].address = (uint64_t)&gdt_tables[cpu_num];
        __asm__ volatile ("lgdt %0" : : "m"(gdt_desc[cpu_num]) : "memory");
        __asm__ volatile ("ltr %0" : : "r"((uint16_t)0x28));
        gdt_reload_seg();
        if (cpu_num == 0)
            kpokln("GDT Loaded.");
    }
}

namespace TSS{
    void SetRSP(uint32_t cpu_num, int32_t rsp, void *stack) {
        tss_desc[cpu_num].rsp[rsp] = (uint64_t)stack;
    }

    void SetIST(uint32_t cpu_num, int32_t ist, void *stack) {
        tss_desc[cpu_num].ist[ist] = (uint64_t)stack;
    }
}