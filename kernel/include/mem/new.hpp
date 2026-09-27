/*
* SPDX-License-Identifier: GPL-2.0-only
* File: new.hpp
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
#include <stddef.h>
#include <klib/klib.h>

/* 修复: 原在头文件内联定义替换版 operator new, clang 报 -Winline-new-delete;
   改为声明于头文件、定义于 src/mem/new.cpp(避免多重定义, 消 222 条警告) */
void *operator new(size_t, void *ptr_) throw();
void *operator new[](unsigned long size);

extern const char* _X__file__;
extern const char* _X__func__;
extern size_t _X__line__;
#define new (_X__func__=__PRETTY_FUNCTION__,_X__file__=__FILE__,_X__line__=__LINE__) && 0 ? NULL : new
