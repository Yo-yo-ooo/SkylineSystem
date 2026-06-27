//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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
};

#endif

