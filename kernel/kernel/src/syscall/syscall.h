/* Copyright 2022, Contributors To LensorOS.
 * All rights reserved.
 *
 * This file is part of LensorOS.
 *
 * LensorOS is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LensorOS is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LensorOS. If not, see <https://www.gnu.org/licenses
 */

#include <stdint.h>
constexpr uintptr_t OS_NUM_SYSCALLS = 2;
extern void* syscalls[OS_NUM_SYSCALLS];

// Defined in `syscalls.cpp`
// Used by `syscalls.asm`
extern uintptr_t num_syscalls;

// Defined in `syscalls.asm`
extern "C" void system_call_handler_asm();
