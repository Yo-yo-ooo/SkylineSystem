/*
* SPDX-License-Identifier: GPL-2.0-only
* File: fb.h
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
#ifndef _FRAME_BUFFER_H_
#define _FRAME_BUFFER_H_

#include <stdint.h>
#include <stddef.h>

namespace FrameBufferDevice{

    uint64_t MemoryMap(
        uint64_t length,uint64_t prot,
        uint64_t offset,uint64_t VADDR
    );

    void Init();
}

#endif