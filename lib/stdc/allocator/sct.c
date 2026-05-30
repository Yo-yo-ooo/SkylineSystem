/*
* SPDX-License-Identifier: GPL-2.0-only
* File: sct.c
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
#include <stdint.h>


#define KB(x) ((x) * 1024ULL)
#define MB(x) ((x) * 1024ULL * 1024ULL)
#define GB(x) ((x) * 1024ULL * 1024ULL * 1024ULL)

/*
 * SizeClassTable 核心映射矩阵
 * [0]: Size (单元规格, 字节数)
 * [1]: FirstLevelListHead (一级总管理块 MCB 链表头指针, 运行期动态初始化为 0)
 * [2]: RegionSize (配合申请的连续内存区大小。根据严格规则：>= 2MB 或是大对象时，此列必须置 0)
 */
uint64_t SizeClassTable[59][3] = {
    // ============================================================================
    // 1. 【小对象分界区】 (Size < 2MB, RegionSize > 0)
    // 配合 16KB 定长二级管理块 (SCB)，在 64 字节元数据下完美对齐，放心拉满高吞吐
    // ============================================================================
    //  { 单元规格Size,  MCB指针,  连续控制区RegionSize }

    // ============================================================================
    // 1. 【极小对象区】 (大幅缩小 RegionSize，减少内存闲置压力)
    // 规则：Step = Size + 64B(元数据)。 
    // 目标：每个 SCB 控制 512 ~ 2048 个槽位即可，不需要几万个。
    // ============================================================================
    { 32ULL,          0,        KB(256) }, 
    { 64ULL,          0,        KB(256) }, 
    { 128ULL,         0,        KB(512) }, 
    { 256ULL,         0,        KB(512) }, 
    { 512ULL,         0,        KB(512) },
    { KB(1),          0,        KB(512)}, // 1KB:  产生 512 槽位
    { KB(2),          0,        KB(256)}, // 2KB:  产生 128 槽位
    { KB(4),          0,        KB(256)}, // 4KB:  产生 64 槽位
    { KB(8),          0,        KB(128)}, // 8KB:  产生 16 槽位
    { KB(16),         0,        KB(128)}, // 16KB: 产生 8 槽位
    { KB(32),         0,        KB(128)}, // 32KB: 触底 128KB 区块 (产生 4 槽位)
    { KB(64),         0,        KB(256)}, // 64KB: 随槽位底线反弹至 256KB 区块 (产生 4 槽位)
    { KB(128),        0,        KB(512)}, // 128KB: 反弹至 512KB 区块
    { KB(256),        0,        MB(1)  }, // 256KB: 反弹至 1MB 区块
    { KB(512),        0,        MB(2)  }, // 512KB: 反弹至 2MB 区块
    { MB(1),          0,        MB(2)  }, // 1MB:  封顶 2MB 小对象区块 (产生 2 槽位, bit_tail = 1)

    // ============================================================================
    // 2. 【大对象分界区】 (Size >= 2MB, RegionSize == 0)
    // 严格规则：第 3 列全部置 0，交由 64 字节的 AllocBlock_t 独立描述物理页
    // ============================================================================
    { MB(2),          0,        0 },      // 2MB: 触发大对象边界，RegionSize置0
    { MB(4),          0,        0 },      // 4MB  -> 物理页计算: (4MB + 64B + 4095) / 4096
    { MB(8),          0,        0 },      // 8MB
    { MB(16),         0,        0 },      // 16MB
    { MB(32),         0,        0 },      // 32MB
    { MB(64),         0,        0 },      // 64MB
    { MB(128),        0,        0 },      // 128MB
    { MB(256),        0,        0 },      // 256MB
    { MB(512),        0,        0 },      // 512MB
    { GB(1),          0,        0 },      // 1GB  (注：若未定义GB宏，请替换为 1073741824ULL)
    { GB(2),          0,        0 },      // 2GB
    { GB(4),          0,        0 },      // 4GB
    { GB(8),          0,        0 },      // 8GB
    { 17179869184ULL, 0,        0 },      // 16GB
    { 34359738368ULL, 0,        0 },      // 32GB
    { 68719476736ULL, 0,        0 },      // 64GB
    { 137438953472ULL, 0,        0 },     // 128GB
    { 274877906944ULL, 0,        0 },     // 256GB
    { 549755813888ULL, 0,        0 },     // 512GB
    { 1099511627776ULL, 0,       0 },     // 1TB
    { 2199023255552ULL, 0,       0 },     // 2TB
    { 4398046511104ULL, 0,       0 },     // 4TB
    { 8796093022208ULL, 0,       0 },     // 8TB
    { 17592186044416ULL, 0,      0 },     // 16TB
    { 35184372088832ULL, 0,      0 },     // 32TB
    { 70368744177664ULL, 0,      0 },     // 64TB
    { 140737488355328ULL, 0,     0 },     // 128TB
    { 281474976710656ULL, 0,     0 },     // 256TB
    { 562949953421312ULL, 0,     0 },     // 512TB
    { 1125899906842624ULL, 0,    0 },     // 1PB
    { 2251799813685248ULL, 0,    0 },     
    { 4503599627370496ULL, 0,    0 },     
    { 9007199254740992ULL, 0,    0 },     
    { 18014398509481984ULL, 0,   0 },    
    { 36028797018963968ULL, 0,   0 },    
    { 72057594037927936ULL, 0,   0 },    
    { 144115188075855872ULL, 0,  0 },   
    { 288230376151711744ULL, 0,  0 },   
    { 576460752303423488ULL, 0,  0 },   
    { 1152921504606846976ULL, 0, 0 },  
    { 2305843009213693952ULL, 0, 0 },  
    { 4611686018427387904ULL, 0, 0 },  
    { 9223372036854775808ULL, 0, 0 }   // 8EB 极限规格，封顶
};