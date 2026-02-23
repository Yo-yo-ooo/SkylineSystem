/*
* SPDX-License-Identifier: GPL-2.0-only
* File: cerrfix.cpp
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
//These function just for compile (idk why)
//This function must not be delete
#include <stdint.h>
extern "C" void _Unwind_Resume(void){/*unrealized!*/}
extern "C" void __cxa_throw_bad_array_new_length(void) {}
extern "C" int32_t __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
extern "C" void __cxa_pure_virtual() { 
    for (;;) {
#ifdef __x86_64__
        asm volatile("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm volatile("wfi");
#elif defined (__loongarch64)
        asm volatile("idle 0");
#endif
    } 
}
extern "C"{void *__dso_handle;}

