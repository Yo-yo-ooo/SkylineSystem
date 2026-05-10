/*
* SPDX-License-Identifier: GPL-2.0-only
* File: prctl.h
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

/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _ASM_X86_PRCTL_H
#define _ASM_X86_PRCTL_H

#define ARCH_SET_GS			0x1001
#define ARCH_SET_FS			0x1002
#define ARCH_GET_FS			0x1003
#define ARCH_GET_GS			0x1004

#define ARCH_GET_CPUID			0x1011
#define ARCH_SET_CPUID			0x1012

#define ARCH_GET_XCOMP_SUPP		0x1021
#define ARCH_GET_XCOMP_PERM		0x1022
#define ARCH_REQ_XCOMP_PERM		0x1023
#define ARCH_GET_XCOMP_GUEST_PERM	0x1024
#define ARCH_REQ_XCOMP_GUEST_PERM	0x1025

#define ARCH_XCOMP_TILECFG		17
#define ARCH_XCOMP_TILEDATA		18

#define ARCH_MAP_VDSO_X32		0x2001
#define ARCH_MAP_VDSO_32		0x2002
#define ARCH_MAP_VDSO_64		0x2003

/* Don't use 0x3001-0x3004 because of old glibcs */

#define ARCH_GET_UNTAG_MASK		0x4001
#define ARCH_ENABLE_TAGGED_ADDR		0x4002
#define ARCH_GET_MAX_TAG_BITS		0x4003
#define ARCH_FORCE_TAGGED_SVA		0x4004

#define ARCH_SHSTK_ENABLE		0x5001
#define ARCH_SHSTK_DISABLE		0x5002
#define ARCH_SHSTK_LOCK			0x5003
#define ARCH_SHSTK_UNLOCK		0x5004
#define ARCH_SHSTK_STATUS		0x5005

/* ARCH_SHSTK_ features bits */
#define ARCH_SHSTK_SHSTK		(1ULL <<  0)
#define ARCH_SHSTK_WRSS			(1ULL <<  1)

#endif /* _ASM_X86_PRCTL_H */
