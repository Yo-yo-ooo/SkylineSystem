/*
* SPDX-License-Identifier: GPL-2.0-only
* File: pinc.h
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

#include <print/e9print.h>


#define WELCOME_X86_64 kinfo("Welcome to the SkylineSystem in x86_64 architecture!\n");
#define WELCOME_AARCH64 kinfo("Welcome to the SkylineSystem in aarch64 architecture!\n");
#define WELCOME_RISCV64 kinfo("Welcome to the SkylineSystem in riscv64 architecture!\n");
#define WELCOME_LOONGARCH64 kinfo("Welcome to the SkylineSystem in loongarch64 architecture!\n");

#define InitFunc(name,func) func;kpok("%s Initialised!\n",name)