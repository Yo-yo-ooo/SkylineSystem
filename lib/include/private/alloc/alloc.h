/*
* SPDX-License-Identifier: GPL-2.0-only
* File: allocator.c
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
#include <private/defs.h>

typedef struct AllocBlock{
    uint64_t AllocSizeAligned; 
    uint64_t AllocSizeUnAligned;
    uint64_t AllocPtrBaseAddress;
    uint64_t BitMapBitLocation; 
    uint64_t BitMapBase;
    uint64_t BlockAddr;
    uint64_t MCBAddr;
}AllocBlock_t;


PACK(typedef struct {
    uint64_t bitmap[2044];      // 16352 字节：提供 130,816 个分配位
    uint64_t is_full;           // 8 字节：[全新改动] 0 表示没满，0xFFFFFFFFFFFFFFFF 表示彻底满了
    uint64_t list_base;         // 8 字节：起始虚拟基地址
    uint64_t bit_tail;          // 8 字节：最后一个有效比特位索引
    uint64_t rem_count;          
}) SecondControlBlock_t;

#define SCB_BITMAP_OFFSET (offsetof(SmallBlockControl_t,bitmap))
#define SCB_IS_FULL_OFFSET (offsetof(SmallBlockControl_t,if_full))
#define SCB_LIST_BASE_OFFSET (offsetof(SmallBlockControl_t,list_base))
#define SCB_BIT_TAIL_OFFSET (offsetof(SmallBlockControl_t,bit_tail))
#define SCB_NEXT_OFFSET (offsetof(SmallBlockControl_t,next))


PACK(typedef struct {
    uint64_t bitmap[32];        // 256 字节：32组位图
    uint8_t  is_full;           // 1 字节：[全新改动] 0表示没满，1表示满了
    uint8_t  padding[7];        // 7 字节：严格对齐填充，保证下面的指针数组 8 字节对齐
    uint64_t list_base[2014];   // 16112 字节：2014个独立大对象虚拟地址插槽 (献祭了1个)
    uint64_t rem_count;
}) LargeSecondControlBlock_t;

#define _SCB_BITMAP_OFFSET (offsetof(BigBlockControl_t,bitmap))
#define _SCB_IS_FULL_OFFSET (offsetof(BigBlockControl_t,if_full))
#define _SCB_PADDING_OFFSET (offsetof(BigBlockControl_t,padding))
#define _SCB_LIST_BASE_OFFSET (offsetof(BigBlockControl_t,list_base))
#define _SCB_NEXT_OFFSET (offsetof(BigBlockControl_t,next))

//主管理链表
//管理SecondControlBlock
PACK(typedef struct {
    uint64_t bitmap[32];        // 256 字节：32组位图
    uint8_t  is_full;           // 1 字节：[全新改动] 0表示没满，1表示满了
    uint8_t  padding[7];        // 7 字节：严格对齐填充，保证下面的指针数组 8 字节对齐
    uint64_t list_base[2014];   // 16112 字节：2014个独立大对象虚拟地址插槽 (献祭了1个)
    uint64_t next;              // 8 字节：死钉在 16376 字节偏置处的单链表指针
}) MainControlBlock_t;

#define MCB_BITMAP_OFFSET (offsetof(BigBlockControl_t,bitmap))
#define MCB_IS_FULL_OFFSET (offsetof(BigBlockControl_t,if_full))
#define MCB_PADDING_OFFSET (offsetof(BigBlockControl_t,padding))
#define MCB_LIST_BASE_OFFSET (offsetof(BigBlockControl_t,list_base))
#define MCB_NEXT_OFFSET (offsetof(BigBlockControl_t,next))

