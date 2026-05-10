/*
* SPDX-License-Identifier: GPL-2.0-only
* File: allin.h
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

#include <klib/kprintf.h>
#include <klib/klib.h>
#include <klib/kio.h>
#include <conf.h>
#include <mem/heap.h>
#include <mem/pmm.h>
#include <arch/x86_64/cpu.h>
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/vmm/vmm.h>
#include <arch/x86_64/interrupt/gdt.h>
#include <arch/x86_64/interrupt/idt.h>
#include <arch/x86_64/dev/pci/pci.h>
#include <arch/x86_64/ioapic/ioapic.h>
#include <arch/x86_64/lapic/lapic.h>
#include <arch/x86_64/pit/pit.h>
/* 
#include <arch/x86_64/MStack/MStackM.h>
#include <arch/x86_64/MStack/MStackS.h> */
#include <arch/x86_64/rtc/rtc.h>
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
