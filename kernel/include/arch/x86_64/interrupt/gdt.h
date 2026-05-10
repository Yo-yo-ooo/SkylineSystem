/*
* SPDX-License-Identifier: GPL-2.0-only
* File: gdt.h
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
#include <pdef.h>

PACK(typedef struct {
    uint16_t len;
    uint16_t base;
    uint8_t base1;
    uint8_t flags;
    uint8_t flags1;
    uint8_t base2;
    uint32_t base3;
    uint32_t resv;
}) tss_entry_t;

PACK(typedef struct {
    uint32_t resv;
    uint64_t rsp[3];
    uint64_t resv1;
    uint64_t ist[7];
    uint64_t resv2;
    uint16_t resv3;
    uint16_t iopb;
}) tss_desc_t;

PACK(typedef struct {
    uint64_t entries[5];
    tss_entry_t tss_entry;
}) gdt_table_t;

PACK(typedef struct {
    uint16_t size;
    uint64_t address;
}) gdt_desc_t;

namespace GDT{
    void Init(uint32_t cpu_num);
}
//void gdt_init(uint32_t cpu_num);

namespace TSS{
    void SetRSP(uint32_t cpu_num, int32_t rsp, void *stack);
    void SetIST(uint32_t cpu_num, int32_t ist, void *stack); 
}
