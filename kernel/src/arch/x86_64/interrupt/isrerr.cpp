/*
* SPDX-License-Identifier: GPL-2.0-only
* File: isrerr.cpp
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

volatile const char* isr_errors[32] = {
    "Division by zero",
    "Debug",
    "Non-maskable interrupt",
    "Breakpoint",
    "Detected overflow",
    "Out-of-bounds",
    "Invalid opcode",
    "No coprocessor",
    "Double fault",
    "Coprocessor segment overrun",
    "Bad TSS",
    "Segment not present",
    "Stack fault",
    "General protection fault",
    "Page fault",
    "Unknown interrupt",
    "Coprocessor fault",
    "Alignment check",
    "Machine check",
    "Reserved ISR ERR NO.19",
    "Reserved ISR ERR NO.20",
    "Reserved ISR ERR NO.21",
    "Reserved ISR ERR NO.22",
    "Reserved ISR ERR NO.23",
    "Reserved ISR ERR NO.24",
    "Reserved ISR ERR NO.25",
    "Reserved ISR ERR NO.26",
    "Reserved ISR ERR NO.27",
    "Reserved ISR ERR NO.28",
    "Reserved ISR ERR NO.29",
    "Reserved ISR ERR NO.30",
    "Reserved ISR ERR NO.31"
};
