/*
* SPDX-License-Identifier: GPL-2.0-only
* File: cstr.cpp
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
#include <klib/klib.h>



char* StrCombine(const char* a, const char* b)
{
    int32_t lenA = strlen(a);
    int32_t lenB = strlen(b);
    
    int32_t totalLen = lenA + lenB;
    char* tempStr = (char*) kmalloc(totalLen + 1);
    tempStr[totalLen] = 0;

    for (int32_t i = 0; i < lenA; i++)
        tempStr[i] = a[i];
    for (int32_t i = 0; i < lenB; i++)
        tempStr[i + lenA] = b[i];

    return tempStr;
}

char intTo_stringOutput[128];
const char *to_string(uint64_t value)
{
    uint8_t size = 0;
    uint64_t sizetest = value;
    while (sizetest / 10 > 0)
    {
        sizetest /= 10;
        size++;
    }

    uint8_t index = 0;
    while (value / 10 > 0)
    {
        uint8_t remainder = value % 10;
        value /= 10;
        intTo_stringOutput[size - index] = remainder + '0';
        index++;
    }
    uint8_t remainder = value % 10;
    intTo_stringOutput[size - index] = remainder + '0';
    intTo_stringOutput[size + 1] = 0;
    intTo_stringOutput[size + 2] = 0;
    return intTo_stringOutput;
}

const char *to_string(int64_t value)
{
    uint8_t size = 0;
    if (value < 0)
    {
        value *= -1;
        intTo_stringOutput[0] = '-';
        size = 1;
    }

    uint64_t sizetest = value;
    while (sizetest / 10 > 0)
    {
        sizetest /= 10;
        size++;
    }

    uint8_t index = 0;
    while (value / 10 > 0)
    {
        uint8_t remainder = value % 10;
        value /= 10;
        intTo_stringOutput[size - index] = remainder + '0';
        index++;
    }

    uint8_t remainder = value % 10;
    intTo_stringOutput[size - index] = remainder + '0';
    intTo_stringOutput[size + 1] = 0;
    intTo_stringOutput[size + 2] = 0;

    return intTo_stringOutput;
}

const char *to_string(int32_t value)
{
    return to_string((int64_t)value);
}

const char *to_string(uint32_t value)
{
    return to_string((int64_t)value);
}

const char *to_string(char value)
{
    intTo_stringOutput[0] = value;
    intTo_stringOutput[1] = 0;
    return intTo_stringOutput;
}

char doubleTo_stringOutput[128];
#ifdef __x86_64__
__ffunc const char *to_string(double value, uint8_t places)
{
    uint8_t size = 0;
    if (value < 0)
    {
        value *= -1;
        doubleTo_stringOutput[0] = '-';
        size = 1;
    }

    uint64_t sizetest = (int64_t)value;
    while (sizetest / 10 > 0)
    {
        sizetest /= 10;
        size++;
    }

    uint8_t index = 0;

    if (places > 0)
    {
        size += places + 1;

        double temp = 1;

        for (int32_t i = 0; i < places; i++)
            temp *= 10;

        uint64_t value3 = (uint64_t)((value - ((uint64_t)value)) * temp);

        for (int32_t i = 0; i < places; i++)
        {
            uint8_t remainder = value3 % 10;
            value3 /= 10;
            doubleTo_stringOutput[size - index] = remainder + '0';
            index++;
        }

        doubleTo_stringOutput[size - index] = '.';
        index++;
    }

    uint64_t value2 = (int64_t)value;
    if (value2 == 0)
        doubleTo_stringOutput[size - index] = '0';
    else
        while (value2 > 0)
        {
            uint8_t remainder = value2 % 10;
            value2 /= 10;
            doubleTo_stringOutput[size - index] = remainder + '0';
            index++;
        }

    doubleTo_stringOutput[size + 1] = 0;

    return doubleTo_stringOutput;
}

const char *to_string(double value)
{
    return to_string(value, 2);
}
#endif

const char *to_string(bool value)
{
    if (value)
        return "true";
    else
        return "false";
}

__ffunc uint32_t ConvertStringToHex(const char *data)
{
    uint32_t hex = 0;

    for (uint32_t i = 0; i < 6;)
    {
        uint8_t temp = 0;
        for (uint32_t i2 = 16; i2 != 0; i2 /= 16)
        {
            if (data[i] >= '0' && data[i] <= '9')
                temp += (data[i] - '0') * i2;
            else if (data[i] >= 'A' && data[i] <= 'F')
                temp += (data[i] + 10 - 'A') * i2;
            else if (data[i] >= 'a' && data[i] <= 'f')
                temp += (data[i] + 10 - 'a') * i2;
            i++;
        }
        hex = hex << 8;
        hex += temp;
    }

    return hex;
}

unsigned long ConvertStringToLongHex(const char *data)
{
    unsigned long hex = 0;

    for (uint32_t i = 0; i < 16; i++)
    {
        if (data[i] == 0)
            break;
        uint8_t temp = 0;
        {
            if (data[i] >= '0' && data[i] <= '9')
                temp += (data[i] - '0');
            else if (data[i] >= 'A' && data[i] <= 'F')
                temp += (data[i] + 10 - 'A');
            else if (data[i] >= 'a' && data[i] <= 'f')
                temp += (data[i] + 10 - 'a');
        }
        hex = hex << 4;
        hex += temp;
    }

    return hex;
}

char hexTo_stringOutput[128];
const char *hexABC = "0123456789ABCDEF";
const char *ConvertHexToString(uint64_t hex, uint8_t size)
{
    hex = (hex << (16 - size) * 4);
    for (uint8_t i = 0; i < size; i++)
        hexTo_stringOutput[i] = hexABC[((hex << i * 4) >> ((15) * 4))];

    hexTo_stringOutput[size] = 0;

    return hexTo_stringOutput;
}
const char *ConvertHexToString(uint64_t hex)
{
    return ConvertHexToString(hex, (64 / 4));
}
const char *ConvertHexToString(uint32_t hex)
{
    return ConvertHexToString(hex, (32 / 4));
}
const char *ConvertHexToString(uint16_t hex)
{
    return ConvertHexToString(hex, (16 / 4));
}
const char *ConvertHexToString(uint8_t hex)
{
    return ConvertHexToString(hex, (8 / 4));
}

int64_t to_int(const char *string)
{
    uint64_t number = 0;
    uint64_t size = 0;
    while (string[size] != 0)
        size++;

    int64_t end = 0;
    int64_t exp = 1;
    if (string[0] == '-')
    {
        end = 1;
        exp *= -1;
    }

    for (int64_t i = size - 1; i >= end; i--)
    {
        number += exp * (string[i] - '0');
        exp *= 10;
    }
    return number;
}
