/*
* SPDX-License-Identifier: GPL-2.0-only
* File: klibc.h
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
#ifndef _KILBC_H_
#define _KILBC_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#include <klib/klib.h>
#else
extern void _memcpy(void* src, void* dest, uint64_t size);
extern void _memset(void* dest, uint8_t value, uint64_t size);
extern void _memmove(void* dest,void* src, uint64_t size);
extern int32_t _memcmp(const void* buffer1,const void* buffer2,size_t  count);

extern void* kmalloc(uint64_t size);
extern void kfree(void* ptr);
extern void* krealloc(void* ptr, uint64_t size);
extern void *kcalloc(size_t numitems, size_t size);
#endif

#endif