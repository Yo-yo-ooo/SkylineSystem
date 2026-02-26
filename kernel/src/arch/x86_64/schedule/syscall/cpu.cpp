/*
* SPDX-License-Identifier: GPL-2.0-only
* File: cpu.cpp
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
#include <klib/kio.h>
#include <arch/x86_64/asm/prctl.h>

uint64_t sys_arch_prctl(uint64_t op, uint64_t extra,uint64_t ign_0, uint64_t ign_1, \
    uint64_t ign_2,uint64_t ign_3) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    IGNORE_VALUE(ign_3);
    uint64_t temp=0;
    uint64_t *pdpt = (uint64_t*)Schedule::this_thread()->pagemap->toplvl[PML4E(extra)];
    if (!PAGE_EXISTS(pdpt)) 
        temp = EFAULT;
    switch (op) {
        case ARCH_SET_FS:
            wrmsr(FS_BASE, extra);
            break;
        case ARCH_SET_GS:
            wrmsr(GS_BASE,extra);
            break;
        case ARCH_GET_FS:
            temp = rdmsr(FS_BASE);
            break;
        case ARCH_GET_GS:
            temp = rdmsr(GS_BASE);
            break;
        case ARCH_GET_CPUID:
        case ARCH_SET_CPUID:
        case ARCH_GET_XCOMP_SUPP:
        case ARCH_GET_XCOMP_PERM:
        case ARCH_REQ_XCOMP_PERM:
        case ARCH_GET_XCOMP_GUEST_PERM:
        case ARCH_REQ_XCOMP_GUEST_PERM:
            break;
        default:
            kerror("[sys_arch_prctl]: %d not implemented.\n", op);
            return -EINVAL;
            break;
    }
    return temp;
}
