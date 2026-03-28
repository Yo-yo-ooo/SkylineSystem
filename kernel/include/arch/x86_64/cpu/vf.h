/*
* SPDX-License-Identifier: GPL-2.0-only
* File: vf.h
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
#ifndef _x86_64_CPU_VF_H_
#define _x86_64_CPU_VF_H_

#include <stdint.h>
#include <stddef.h>

void WRFSBASE_V0(uint64_t value);
void WRFSBASE_V1(uint64_t value);


#endif