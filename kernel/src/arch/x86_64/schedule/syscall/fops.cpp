/*
* SPDX-License-Identifier: GPL-2.0-only
* File: fops.cpp
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
#include <fs/fd.h>

uint64_t sys_fread(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);

}

uint64_t sys_fwrite(uint64_t fd_idx, uint64_t buf, uint64_t count, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
        IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);

}

int64_t sys_flseek(uint64_t fd_idx, uint64_t offset, uint64_t whence, \
    uint64_t ign_0,uint64_t ign_1,uint64_t ign_2){

}


uint64_t sys_fopen(uint64_t path, uint64_t flags, uint64_t mode, \
    GENERATE_IGN3()) {

}

uint64_t sys_fclose(uint64_t fd,GENERATE_IGN5()){
    IGNV_5();

}

uint64_t sys_mkdir(uint64_t path,uint64_t mode,GENERATE_IGN4()){
    IGNV_4();

}

