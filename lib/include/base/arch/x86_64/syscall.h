/*
* SPDX-License-Identifier: GPL-2.0-only
* File: syscall.h
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

#ifndef _X86_64_SYSCALL_H_
#define _X86_64_SYSCALL_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>


#define syscall(num, a1, a2, a3, a4, a5, a6) ({ \
    long _ret; \
    register long _rax asm("rax") = (long)(num); \
    register long _rdi asm("rdi") = (long)(a1);  \
    register long _rsi asm("rsi") = (long)(a2);  \
    register long _rdx asm("rdx") = (long)(a3);  \
    register long _r10 asm("r10") = (long)(a4); \
    register long _r8  asm("r8")  = (long)(a5);  \
    register long _r9  asm("r9")  = (long)(a6);  \
    asm volatile("syscall" \
        : "+r"(_rax) \
        : "r"(_rdi), "r"(_rsi), "r"(_rdx), \
          "r"(_r10), "r"(_r8), "r"(_r9) \
        : "rcx", "r11", "memory" \
    ); \
    _ret = _rax; \
    _ret; \
})

uint64_t sys_mmap(uint64_t addr,uint64_t length, uint64_t mode,uint64_t flags,uint64_t offset);
uint64_t sys_munmap(uint64_t addr,uint64_t length);
uint64_t sys_fread(int32_t fd_idx, uint64_t buf, uint64_t count);
uint64_t sys_fwrite(int32_t fd_idx, uint64_t buf, uint64_t count);
uint64_t sys_flseek(int32_t fd_idx, uint64_t offset, uint64_t whence);
uint64_t sys_fopen(uint64_t path, uint64_t flags);
uint64_t sys_fclose(int32_t fd);

#ifdef __cplusplus
}
#endif

#endif