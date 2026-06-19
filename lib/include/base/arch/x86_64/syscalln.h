/*
* SPDX-License-Identifier: GPL-2.0-only
* File: syscalln.h
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
#ifndef _X86_64_SYSCALLN_H_
#define _X86_64_SYSCALLN_H_

#define SYSCALL_FOPEN 0
#define SYSCALL_FWRITE 1
#define SYSCALL_FREAD 2
#define SYSCALL_FCLOSE 3
#define SYSCALL_FLSEEK 4

#define SYSCALL_MMAP 13
#define SYSCALL_MUNMAP 12

#endif