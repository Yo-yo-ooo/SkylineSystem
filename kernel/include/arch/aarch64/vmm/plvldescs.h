/*
* SPDX-License-Identifier: GPL-2.0-only
* File: plvldescs.h
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
#ifndef _AARCH64_PAGE_LEVEL_DESC_H_
#define _AARCH64_PAGE_LEVEL_DESC_H_

#include <stdint.h>
#include <stddef.h>
#include <pdef.h>
#include <conf.h>


PACK(typedef struct __u64_vaddr_4K{
    uint16_t BlockOffset : 12;
    uint16_t LV3_TblIndex : 9; // Level 3 Table Index
    uint16_t LV2_TblIndex : 9; // Level 2 Table Index
    uint16_t LV1_TblIndex : 9; // Level 1 Table Index
    uint16_t LV0_TblIndex : 9; // Level 0 Table Index
    uint16_t Reserved        ;
})__u64_vaddr_4K;

PACK(typedef struct __u64_vaddr_16K{
    uint16_t BlockOffset  : 14;
    uint16_t LV3_TblIndex : 11;
    uint16_t LV2_TblIndex : 11;
    uint16_t LV1_TblIndex : 11;
    uint8_t  LV0_TblIndex : 1 ;
    uint16_t Reserved         ;
})__u64_vaddr_16K;

PACK(typedef struct __u64_vaddr_64K{
    uint16_t BlockOffset      ;
    uint16_t LV3_TblIndex : 13;
    uint16_t LV2_TblIndex : 13;
    uint16_t LV1_TblIndex : 6 ;
    uint16_t Reserved         ;
})__u64_vaddr_64K;

PACK(typedef struct __tdesc_4K{
    uint8_t  Valid      : 1 ;
    uint8_t  Type       : 1 ;
    uint8_t  Rsvd       : 10;
    uint64_t NextLvl    : 36;
    uint8_t  RES0       : 4 ;
    uint8_t  Reserved   : 7 ;
    uint8_t  PXNTbl     : 1 ;
    uint8_t  XNTbl      : 1 ;
    uint8_t  APTbl      : 2 ;
    uint8_t  NATbl      : 1 ;
})__tdesc_4K;

PACK(typedef struct __tdesc_16K{
    uint8_t  Valid      : 1 ;
    uint8_t  Type       : 1 ;
    uint8_t  Rsvd       : 12;
    uint64_t NextLvl    : 34;
    uint8_t  RES0       : 4 ;
    uint8_t  Reserved   : 7 ;
    uint8_t  PXNTbl     : 1 ;
    uint8_t  XNTbl      : 1 ;
    uint8_t  APTbl      : 2 ;
    uint8_t  NATbl      : 1 ;
})__tdesc_16K;

PACK(typedef struct __tdesc_64K{
    uint8_t  Valid      : 1 ;
    uint8_t  Type       : 1 ;
    uint8_t  Rsvd       : 14;
    uint32_t NextLvl        ;
    uint8_t  RES0       : 4 ;
    uint8_t  Reserved   : 7 ;
    uint8_t  PXNTbl     : 1 ;
    uint8_t  XNTbl      : 1 ;
    uint8_t  APTbl      : 2 ;
    uint8_t  NATbl      : 1 ;
})__tdesc_64K;
PACK(typedef struct __bdesc_4K_LV1{
    uint8_t  Valid               : 1 ;
    uint8_t  Type                : 1 ;
    uint16_t LowerBlockAttr      : 10;
    uint16_t RES0_2              : 18;
    uint32_t OutputAddr          : 18;
    uint8_t  RES0                : 4 ;
    uint16_t UpperBlockAttr      : 12;
})__bdesc_4K_LV1;

PACK(typedef struct __bdesc_4K_LV2{
    uint8_t  Valid               : 1 ;
    uint8_t  Type                : 1 ;
    uint16_t LowerBlockAttr      : 10;
    uint16_t RES0_2              : 9 ;
    uint32_t OutputAddr          : 27;
    uint8_t  RES0                : 4 ;
    uint16_t UpperBlockAttr      : 12;
})__bdesc_4K_LV2;

PACK(typedef struct __bdesc_16K_LV2{
    uint8_t  Valid               : 1 ;
    uint8_t  Type                : 1 ;
    uint16_t LowerBlockAttr      : 10;
    uint16_t RES0_2              : 13;
    uint32_t OutputAddr          : 23;
    uint8_t  RES0                : 4 ;
    uint16_t UpperBlockAttr      : 12;
})__bdesc_16K_LV2;

PACK(typedef struct __bdesc_64K_LV2{
    uint8_t  Valid               : 1 ;
    uint8_t  Type                : 1 ;
    uint16_t LowerBlockAttr      : 10;
    uint16_t RES0_2              : 17;
    uint32_t OutputAddr          : 19;
    uint8_t  RES0                : 4 ;
    uint16_t UpperBlockAttr      : 12;
})__bdesc_64K_LV2;

union Translate4K{
    uint64_t vaddr;
    __u64_vaddr_4K addrdesc;
};

union Translate16K{
    uint64_t vaddr;
    __u64_vaddr_16K addrdesc;
};

union Translate64K{
    uint64_t vaddr;
    __u64_vaddr_64K addrdesc;
};

union Pair4K{
    __tdesc_4K desc;
    
};

PACK(typedef struct field_4KPD{
    uint64_t UpperAttrs    : 12; // Bits [63:52]: 高位属性 (PXN, UXN, etc.)
    uint64_t RES0           : 4;  // Bits [51:48]: 只有开启 TCR_EL1.DS 时有效
    uint64_t OutputAddr    : 36; // Bits [47:12]: 输出地址
    uint64_t LowerAttrs    : 10; // Bits [11:2]: 属性位 (AF, SH, AP, etc.)
    uint64_t Type           : 1;  // Bit 1: 1 为 Page, 0 为 Block
    uint64_t Valid          : 1;  // Bit 0: 必须为 1
}) field_4KPD;

PACK(typedef struct field_16KPD{
    uint64_t UpperAttrs    : 12; // Bits [63:52]: 高位属性 (PXN, UXN, etc.)
    uint64_t RES0           : 4;  // Bits [51:48]: 只有开启 TCR_EL1.DS 时有效
    uint64_t OutputAddr    : 34; // Bits [47:12]: 输出地址
    uint64_t RES0_12_13     : 2;  // Bits [13:12]: 图中 ‡ 标注的 RES0
    uint64_t LowerAttrs    : 10; // Bits [11:2]: 属性位 (AF, SH, AP, etc.)
    uint64_t Type           : 1;  // Bit 1: 1 为 Page, 0 为 Block
    uint64_t Valid          : 1;  // Bit 0: 必须为 1
}) field_16KPD;

PACK(typedef struct field_64KPD{
    uint64_t UpperAttrs     : 12; // Bits [63:52]: 高位属性 (PXN, UXN, etc.)
    uint64_t RES0           : 4;  // Bits [51:48]: 只有开启 TCR_EL1.DS 时有效
    uint32_t OutputAddr        ; // Bits [47:12]: 输出地址
    uint64_t RES0_           : 4;  // Bits [13:12]: 图中 ‡ 标注的 RES0
    uint64_t LowerAttrs     : 10; // Bits [11:2]: 属性位 (AF, SH, AP, etc.)
    uint64_t Type           : 1;  // Bit 1: 1 为 Page, 0 为 Block
    uint64_t Valid          : 1;  // Bit 0: 必须为 1
}) field_64KPD;

union PageDescriptor4K {
    uint64_t raw;
    field_4KPD fields;
};

union PageDescriptor16K {
    uint64_t raw;
    field_16KPD fields;
};

union PageDescriptor64K {
    uint64_t raw;
    field_64KPD fields;
};

union TableDesc4KLV1{
    __tdesc_4K desc;
    __bdesc_4K_LV1 lv1desc;
};

union TableDesc4KLV2{
    __tdesc_4K desc;
    __bdesc_4K_LV2 lv2desc;
};

#endif