/*
* SPDX-License-Identifier: GPL-2.0-only
* File: init.cpp
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
#include <klib/kprintf.h>
#include <arch/pinc.h>
#include <arch/aarch64/cpu/features.h>
#include <mem/pmm.h>
#include <arch/aarch64/vmm/vmm.h>


void aarch64_init(void){
    WELCOME_AARCH64
    CollectCPUFeatures();
    kpokln("Collected CPU Features in aarch64 architecture.!");
    kinfoln("Current EL:%d",GetCurrentEL());
    InitFunc("PMM",PMM::Init());
    InitFunc("VMM",VMM::Init());

    kpokln("KERNEL STARTED!");
}