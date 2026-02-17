/*
* SPDX-License-Identifier: GPL-2.0-only
* File: lapic.h
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

#include <klib/klib.h>

#define LAPIC_PPR 0x00a0

#define LAPIC_ICRLO 0x0300
#define LAPIC_ICRHI 0x0310

#define LAPIC_ICINI 0x0500
#define LAPIC_ICSTR 0x0600

#define LAPIC_ICEDGE 0x0000

#define LAPIC_ICPEND 0x00001000
#define LAPIC_ICPHYS 0x00000000
#define LAPIC_ICASSR 0x00004000
#define LAPIC_ICSHRTHND 0x00000000
#define LAPIC_ICDESTSHIFT 24

#define LAPIC_ICRAIS 0x00080000
#define LAPIC_ICRAES 0x000c0000

// delivery mode
#define APIC_DeliveryMode_Fixed 			0x0
#define APIC_DeliveryMode_LowestPriority 0x1
#define APIC_DeliveryMode_SMI 			0x2
#define APIC_DeliveryMode_NMI 			0x4
#define APIC_DeliveryMode_INIT 			0x5
#define APIC_DeliveryMode_Startup 		0x6
#define APIC_DeliveryMode_ExtINT 		0x7

// Timer

#define LAPIC_TIMER_DIV 0x3E0
#define LAPIC_TIMER_INITCNT 0x380
#define LAPIC_TIMER_LVT 0x320
#define LAPIC_TIMER_DISABLE 0x10000
#define LAPIC_TIMER_CURCNT 0x390
#define LAPIC_TIMER_PERIODIC 0x20000

#define LAPIC_SUPPORT_FLAG_X2Apic			(1ull << 0)
#define LAPIC_SUPPORT_FLAG_EOIBroadcase	    (1ull << 1)

// mask
#define APIC_Mask_Unmasked 	0x0
#define APIC_Mask_Masked 	0x1

// trigger mode
#define APIC_TriggerMode_Edge 	0x0
#define APIC_TriggerMode_Level 	0x1

// delivery status
#define APIC_DeliveryStatus_Idle 	0x0
#define APIC_DeliveryStatus_Pending 	0x1

// destination shorthand
#define APIC_DestShorthand_None 			0x0
#define APIC_DestShorthand_Self 			0x1
#define APIC_DestShorthand_AllIncludingSelf 0x2
#define APIC_DestShorthand_AllExcludingSelf 0x3

// destination mode
#define APIC_DestMode_Physical 	0x0
#define APIC_DestMode_Logical 	0x1

// level
#define APIC_Level_Deassert	0x0
#define APIC_Level_Assert 	0x1


namespace LAPIC{

    extern uint64_t apic_ticks;
    extern bool x2apic;

    void Init();

    void Write(uint32_t reg, uint64_t val);
    uint64_t Read(uint32_t reg);
    uint32_t GetID();
    void EOI();

    void StopTimer();
    void Oneshot(uint8_t vec, uint64_t ms);
    void CalibrateTimer();
    uint64_t InitTimer();

    void IPI(uint32_t id, uint32_t dat);
    void IPIAll(uint32_t lapic_id, uint32_t vector);
    void IPIOthers(uint32_t lapic_id, uint32_t vector);
}