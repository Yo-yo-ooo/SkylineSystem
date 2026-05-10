/*
* SPDX-License-Identifier: GPL-2.0-only
* File: vmm.h
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
#ifndef _AARCH64_VMM_H
#define _AARCH64_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <arch/aarch64/vmm/plvldescs.h>

typedef struct _pagemap_t{
    uint32_t   levels;
    void *top_level[2]; // 0:LowerRoot,1:HigherRoot!
}pagemap_t;

enum page_size {
    Size4KiB,
    Size2MiB,
    Size1GiB
};

#define UXN (1ULL << 54) //User mode cannot Run
#define PXN (1ULL << 53) //Kernel mode cnnot run(data)

namespace VMM
{
    extern volatile pagemap_t *KernelPageMap;
    void Init();
    void SwitchPM(pagemap_t *pagemap);
} // namespace VMM


#endif