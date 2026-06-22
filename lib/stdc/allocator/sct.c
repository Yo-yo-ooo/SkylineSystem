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

#pragma GCC push_options
#pragma GCC optimize ("O0")
/*
 * SizeClassTable 核心映射矩阵 (共 75 档)
 * [0]: Size (单元规格, 字节数)
 * [1]: FirstLevelListHead (一级总管理块 MCB 链表头指针, 运行期动态初始化为 0)
 * [2]: RegionSize (配合申请的连续内存区大小。根据严格规则：>= 2MB 或是大对象时，此列必须置 0)
 */
__attribute__((aligned(64)))
volatile uint64_t SizeClassTable[75][3] = {
    // ============================================================================
    // 1. 【小对象分界区】 (Size <= 2MB, RegionSize > 0)
    // 引入 1.5倍 步长的亚规格（Sub-classes），极大降低内存碎片
    // ============================================================================
    { 32ULL,          0, KB(256) }, // idx 0: 32B
    { 48ULL,          0, KB(256) }, // idx 1: 48B (亚规格)
    { 64ULL,          0, KB(256) }, // idx 2: 64B
    { 96ULL,          0, KB(512) }, // idx 3: 96B (亚规格)
    { 128ULL,         0, KB(512) }, // idx 4: 128B
    { 192ULL,         0, KB(512) }, // idx 5: 192B (亚规格)
    { 256ULL,         0, KB(512) }, // idx 6: 256B
    { 384ULL,         0, KB(512) }, // idx 7: 384B (亚规格)
    { 512ULL,         0, KB(512) }, // idx 8: 512B
    { 768ULL,         0, KB(512) }, // idx 9: 768B (亚规格)
    { KB(1),          0, KB(512) }, // idx 10: 1KB
    { KB(1) + 512ULL, 0, KB(256) }, // idx 11: 1.5KB (亚规格)
    { KB(2),          0, KB(256) }, // idx 12: 2KB
    { KB(3),          0, KB(256) }, // idx 13: 3KB (亚规格)
    { KB(4),          0, KB(256) }, // idx 14: 4KB
    { KB(6),          0, KB(128) }, // idx 15: 6KB (亚规格)
    { KB(8),          0, KB(128) }, // idx 16: 8KB
    { KB(12),         0, KB(128) }, // idx 17: 12KB (亚规格)
    { KB(16),         0, KB(128) }, // idx 18: 16KB
    { KB(24),         0, KB(128) }, // idx 19: 24KB (亚规格)
    { KB(32),         0, KB(128) }, // idx 20: 32KB
    { KB(48),         0, KB(256) }, // idx 21: 48KB (亚规格)
    { KB(64),         0, KB(256) }, // idx 22: 64KB
    { KB(96),         0, KB(512) }, // idx 23: 96KB (亚规格)
    { KB(128),        0, KB(512) }, // idx 24: 128KB
    { KB(192),        0, MB(1)   }, // idx 25: 192KB (亚规格)
    { KB(256),        0, MB(1)   }, // idx 26: 256KB
    { KB(384),        0, MB(2)   }, // idx 27: 384KB (亚规格)
    { KB(512),        0, MB(2)   }, // idx 28: 512KB
    { KB(768),        0, MB(2)   }, // idx 29: 768KB (亚规格)
    { MB(1),          0, MB(2)   }, // idx 30: 1MB
    { MB(1) + KB(512),0, MB(2)   }, // idx 31: 1.5MB (亚规格)

    // ============================================================================
    // 2. 【大对象分界区】 (Size >= 2MB, RegionSize == 0)
    // 严格规则：第 3 列全部置 0，恢复纯 2 的幂次方扩张，交由 AllocBlock_t 独立描述
    // ============================================================================
    { MB(2),          0, 0 },       // idx 32: 2MB 触发大对象边界，RegionSize置0
    { MB(4),          0, 0 },       // idx 33: 4MB
    { MB(8),          0, 0 },       // idx 34: 8MB
    { MB(16),         0, 0 },
    { MB(32),         0, 0 },
    { MB(64),         0, 0 },
    { MB(128),        0, 0 },
    { MB(256),        0, 0 },
    { MB(512),        0, 0 },
    { GB(1),          0, 0 },
    { GB(2),          0, 0 },
    { GB(4),          0, 0 },
    { GB(8),          0, 0 },
    { 17179869184ULL, 0, 0 },       // 16GB
    { 34359738368ULL, 0, 0 },
    { 68719476736ULL, 0, 0 },
    { 137438953472ULL, 0, 0 },
    { 274877906944ULL, 0, 0 },
    { 549755813888ULL, 0, 0 },
    { 1099511627776ULL, 0, 0 },      // 1TB
    { 2199023255552ULL, 0, 0 },
    { 4398046511104ULL, 0, 0 },
    { 8796093022208ULL, 0, 0 },
    { 17592186044416ULL, 0, 0 },
    { 35184372088832ULL, 0, 0 },
    { 70368744177664ULL, 0, 0 },
    { 140737488355328ULL, 0, 0 },
    { 281474976710656ULL, 0, 0 },
    { 562949953421312ULL, 0, 0 },
    { 1125899906842624ULL, 0, 0 },   // 1PB
    { 2251799813685248ULL, 0, 0 },
    { 4503599627370496ULL, 0, 0 },
    { 9007199254740992ULL, 0, 0 },
    { 18014398509481984ULL, 0, 0 },
    { 36028797018963968ULL, 0, 0 },
    { 72057594037927936ULL, 0, 0 },
    { 144115188075855872ULL, 0, 0 },
    { 288230376151711744ULL, 0, 0 },
    { 576460752303423488ULL, 0, 0 },
    { 1152921504606846976ULL, 0, 0 }, // 1EB
    { 2305843009213693952ULL, 0, 0 },
    { 4611686018427387904ULL, 0, 0 },
    { 9223372036854775808ULL, 0, 0 }  // 8EB 极限规格，封顶 (idx 74)
};


#pragma GCC pop_options