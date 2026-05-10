/*
* SPDX-License-Identifier: GPL-2.0-only
* File: math.c
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

#include <math.h>

float sqrt(float x) {
#ifdef __x86_64__
    float buf;
    __asm__ __volatile__ ("sqrtss %1, %0" : "=x"(buf) : "x"(x));
    return buf;
#else
    uint32_t i; // 显式使用 32 位无符号整数
    float x2, y;
    const float threehalfs = 1.5F;

    x2 = x * 0.5F;
    y = x;
    // 使用 union 或 memcpy 避开严格别名规则 (Strict Aliasing)
    union { float f; uint32_t i; } conv = { .f = y };
    conv.i = 0x5f375a86 - (conv.i >> 1);
    y = conv.f;
    
    y = y * (threehalfs - (x2 * y * y)); // 1st iteration
    return 1.0F / y;
#endif
}

const float hollyst = 0.017453292519943295769236907684886;


const float sin_table[] = {
    0.0,                                    //sin(0)
    0.17364817766693034885171662676931 ,    //sin(10)
    0.34202014332566873304409961468226 ,    //sin(20)
    0.5 ,                                   //sin(30)
    0.64278760968653932632264340990726 ,    //sin(40)
    0.76604444311897803520239265055542 ,    //sin(50)
    0.86602540378443864676372317075294 ,    //sin(60)
    0.93969262078590838405410927732473 ,    //sin(70)
    0.98480775301220805936674302458952 ,    //sin(80)
    1.0                                     //sin(90)
};

const float cos_table[] = {
    1.0 ,                                   //cos(0)
    0.99984769515639123915701155881391 ,    //cos(1)
    0.99939082701909573000624344004393 ,    //cos(2)
    0.99862953475457387378449205843944 ,    //cos(3)
    0.99756405025982424761316268064426 ,    //cos(4)
    0.99619469809174553229501040247389 ,    //cos(5)
    0.99452189536827333692269194498057 ,    //cos(6)
    0.99254615164132203498006158933058 ,    //cos(7)
    0.99026806874157031508377486734485 ,    //cos(8)
    0.98768834059513772619004024769344      //cos(9)
};


float sin(float x){
#ifndef __x86_64__
    x = x * 180 / pi; // rad to deg
    int32_t sig = 0;

    if(x > 0.0){
        while(x >= 360.0) {
            x = x - 360.0;
        }
    }else{
        while(x < 0.0) {
            x = x + 360.0;
        }
    }

    if(x >= 180.0){
        sig = 1;
        x = x - 180.0;
    }

    x = (x > 90.0) ? (180.0 - x) : x;

    int32_t a = x * 0.1;
    float b = x - 10 * a;
    
    float y = sin_table[a] * cos_table[(int32_t)b] + b * hollyst * sin_table[9 - a];

    return (sig > 0) ? -y : y;
#else
    float ans;
    asm volatile("fsin" : "=t"(ans) : "0"(x));
    return ans;
#endif
}

float cos(float x){
#ifndef __x86_64__
    return sin(90.0 + x * 180 / pi);
#else
    float ans;
    asm volatile("fcos" : "=t"(ans) : "0"(x));
    return ans;
#endif
}