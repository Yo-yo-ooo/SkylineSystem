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


// [0]: Size (单元规格, 字节)
// [1]: FirstLevelListHead (一级总管理块链表头指针, 初始为0)
// [2]: RegionSize (配合申请的连续内存区大小, >=2MB 时置0)
uint64_t SizeClassTable[59][3] = {
    // ================= [ < 2MB 小对象分界 ] =================
    // 配合 16KB 定长管理块 (最大支持130,816槽位)，放心拉满 2MB 高吞吐
    {32ULL, 0, 2097152ULL},         // 32B: 恢复 2MB 区块 (产生 65536 槽位，bit_tail=65535)
    {64ULL, 0, 2097152ULL},         // 64B: 2MB 区块 (产生 32768 槽位，bit_tail=32767)
    {128ULL, 0, 1048576ULL},        // 128B: 1MB 区块 (产生 8192 槽位)
    {256ULL, 0, 1048576ULL},        // 256B: 1MB 区块 (产生 4096 槽位)
    {512ULL, 0, 524288ULL},         // 512B: 512KB 区块
    {1024ULL, 0, 524288ULL},        // 1KB: 512KB 区块
    {2048ULL, 0, 262144ULL},        // 2KB: 256KB 区块
    {4096ULL, 0, 262144ULL},        // 4KB: 256KB 区块
    {8192ULL, 0, 131072ULL},        // 8KB: 128KB 区块
    {16384ULL, 0, 131072ULL},       // 16KB: 128KB 区块
    {32768ULL, 0, 131072ULL},       // 32KB: 触底 128KB 区块
    {65536ULL, 0, 262144ULL},       // 64KB: 随槽位底线反弹至 256KB 区块
    {131072ULL, 0, 524288ULL},      // 128KB: 512KB 区块
    {262144ULL, 0, 1048576ULL},     // 256KB: 1MB 区块
    {524288ULL, 0, 2097152ULL},     // 512KB: 2MB 区块
    {1048576ULL, 0, 2097152ULL},    // 1MB: 封顶 2MB 区块 (产生 2 槽位，bit_tail=1)

    // ================= [ >= 2MB 大对象分界 ] =================
    // 严格规则：>= 2MB 全部置 0，交由 AllocBlock_t 独立描述
    {2097152ULL, 0, 0},             // 2MB: 触发边界，置0
    {4194304ULL, 0, 0},             // 4MB
    {8388608ULL, 0, 0},             // 8MB
    {16777216ULL, 0, 0},            // 16MB
    {33554432ULL, 0, 0},            // 32MB
    {67108864ULL, 0, 0},            // 64MB
    {134217728ULL, 0, 0},           // 128MB
    {268435456ULL, 0, 0},           // 256MB
    {536870912ULL, 0, 0},           // 512MB
    {1073741824ULL, 0, 0},          // 1GB
    {2147483648ULL, 0, 0},          // 2GB
    {4294967296ULL, 0, 0},          // 4GB
    {8589934592ULL, 0, 0},          // 8GB
    {17179869184ULL, 0, 0},         // 16GB
    {34359738368ULL, 0, 0},         // 32GB
    {68719476736ULL, 0, 0},         // 64GB
    {137438953472ULL, 0, 0},        // 128GB
    {274877906944ULL, 0, 0},        // 256GB
    {549755813888ULL, 0, 0},        // 512GB
    {1099511627776ULL, 0, 0},       // 1TB
    {2199023255552ULL, 0, 0},       // 2TB
    {4398046511104ULL, 0, 0},       // 4TB
    {8796093022208ULL, 0, 0},       // 8TB
    {17592186044416ULL, 0, 0},      
    {35184372088832ULL, 0, 0},      
    {70368744177664ULL, 0, 0},      
    {140737488355328ULL, 0, 0},     
    {281474976710656ULL, 0, 0},     
    {562949953421312ULL, 0, 0},     
    {1125899906842624ULL, 0, 0},    
    {2251799813685248ULL, 0, 0},    
    {4503599627370496ULL, 0, 0},    
    {9007199254740992ULL, 0, 0},    
    {18014398509481984ULL, 0, 0},   
    {36028797018963968ULL, 0, 0},   
    {72057594037927936ULL, 0, 0},   
    {144115188075855872ULL, 0, 0},  
    {288230376151711744ULL, 0, 0},  
    {576460752303423488ULL, 0, 0},  
    {1152921504606846976ULL, 0, 0}, 
    {2305843009213693952ULL, 0, 0}, 
    {4611686018427387904ULL, 0, 0}, 
    {9223372036854775808ULL, 0, 0}
};