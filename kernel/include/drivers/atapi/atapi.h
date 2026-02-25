/*
* SPDX-License-Identifier: GPL-2.0-only
* File: atapi.h
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
#ifndef _DRIVER_ATAPI_H_
#define _DRIVER_ATAPI_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>

class ATAPI{
private:
    uint16_t Port;
public:
    ATAPI(uint16_t port):Port(port){}
    uint8_t Read(bool slave, uint32_t lba, uint32_t sectors, uint16_t *buffer);
    uint8_t Write(bool slave, uint32_t lba, uint32_t sectors, uint16_t *buffer);
    ~ATAPI();
}

#endif

