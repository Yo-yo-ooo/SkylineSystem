/*
* SPDX-License-Identifier: GPL-2.0-only
* File: alloc.h
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
#include <stddef.h>
#include <base/defs.h>

#define ALLOC_BLOCK_MAGIC 0x536B796C696E6521ULL // "Skyline!"

typedef struct AllocBlock{
    uint64_t Magic;
    uint64_t AllocSizeAligned; 
    uint64_t AllocSizeUnAligned;
    uint64_t AllocPtrBaseAddress;
    PACK(struct{
        uint32_t SCBBitLocation;
        uint32_t MCBBitLocation;
    })BitMapBitLocation;
    uint64_t BitMapBase;
    uint64_t BlockAddr;
    uint64_t MCBAddr;
}AllocBlock_t;

PACK(typedef struct {
    alignas(8) uint64_t bitmap[2044];      // 提供 130,816 个分配位
    uint64_t is_full;                      // 0 表示没满，1 表示彻底满了
    uint64_t list_base;                    // 起始虚拟基地址
    uint32_t bit_tail;                     // 最后一个有效比特位索引
    uint32_t rem_count;                    // 剩余可用对象计数      
    uint64_t step_size;
}) SecondControlBlock_t;




PACK(typedef struct {
    alignas(8) uint64_t bitmap[32];        // 32组位图 (256字节)
    uint64_t is_full;                      // 直接提升为 64 位，天然消除对齐空洞，业务逻辑无缝兼容
    uint64_t list_base[2014];              // 2014个独立大对象虚拟地址插槽 (16112字节)
    uint64_t rem_count;                    // 剩余计数 (8字节)
}) LargeSecondControlBlock_t;

const uint64_t x = sizeof(LargeSecondControlBlock_t);


//主管理链表
//管理SecondControlBlock
PACK(typedef struct {
    alignas(8) uint64_t bitmap[32];        // 32组位图 (256字节)
    uint64_t list_base[2014];              // 2014个插槽 (16112字节)
    uint64_t next;                         // 下一个 MCB 指针 (8字节)
    uint32_t rem_scb_count;                // 剩余 SCB 计数 (4字节，保持给原子减法 __a_subu32 使用)
    uint8_t  is_full;                      // 满了的状态位 (1字节)
    uint8_t  padding[3];                   // 显式精准填充 3 字节，与上面的 4+1 凑成 8 字节完美收尾！
}) MainControlBlock_t;
