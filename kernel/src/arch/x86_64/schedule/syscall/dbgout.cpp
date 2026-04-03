/*
* SPDX-License-Identifier: GPL-2.0-only
* File: dbgout.cpp
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
#include <arch/x86_64/cpu.h>
#include <mem/pmm.h>

spinlock_t dbgout_lock = 0;

uint64_t sys_dbgsout(uint64_t CharsAddr,uint64_t OutSize,GENERATE_IGN4()){
    IGNV_4();
    if(!is_user_range(CharsAddr,OutSize))
        return -EFAULT;
    spinlock_lock(&dbgout_lock);
    kinfo("App Serial Output: ");
    char *buf = (char*)CharsAddr;
    for(size_t i = 0;i < OutSize;i++){
        Serial::_Write(*(buf + i));
    }
    kprintf("\n");
    spinlock_unlock(&dbgout_lock);
    return OutSize;
}