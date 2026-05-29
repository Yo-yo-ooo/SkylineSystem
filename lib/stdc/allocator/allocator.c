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
#include <private/alloc/alloc.h>
#include <stdc/string.h>

extern uint64_t SizeClassTable[59][3];


#define ALIGN_NEXT_POW2_64(size) \
    ((size) <= 64ULL ? 64ULL : (1ULL << (64 - __builtin_clzll((uint64_t)((size) - 1)))))
#define GET_POW2_64(AlignedSize) (63 - __builtin_clzll(AlignedSize))


void* MoreCore(uint64_t PageCount){(void)PageCount;/*Not IMPLED YET...*/return NULL;}

#define DIV_ROUND_UP(x, y) (((x) + ((y) - 1)) / (y))


// ================= SkylineSystem Malloc 主入口 =================
void* skyline_malloc(size_t size) {
    if (size == 0) return NULL;

    int idx = ALIGN_NEXT_POW2_64(size);
    if (idx == -1) return NULL;

    uint64_t size_class   = SizeClassTable[idx][0];
    uint64_t region_size  = SizeClassTable[idx][2];

    // ---------------------------------------------------------------
    // 1. 小对象分配分支 (< 2MB)
    // ---------------------------------------------------------------
    if (region_size > 0) {
        SecondControlBlock_t* scb = (SecondControlBlock_t*)SizeClassTable[idx][1];
        
        // 快速跳过已满的管理块 (O(1) 判定)
        while (scb && scb->is_full == 0xFFFFFFFFFFFFFFFFULL) {
            scb = (SecondControlBlock_t*)scb->padding; // 沿 padding(next) 链表向后查找
        }

        // 当前规格的一级链表为空或全部爆满 -> 动态向内核申请一套全新的元数据与数据区
        if (!scb) {
            scb = (SecondControlBlock_t*)MoreCore(4); // 16KB 元数据块 = 4页
            if (!scb) return NULL;

            void* data_region = MoreCore(region_size / 4096); // 申请数据连续区
            if (!data_region) return NULL;

            // 初始化新 SCB
            scb->is_full = 0;
            scb->list_base = (uint64_t)data_region;
            scb->bit_tail = (region_size / size_class) - 1;
            for (int i = 0; i < 2044; i++) scb->bitmap[i] = 0;

            // 头插法强挂到 Column[1] 的根锚点上
            scb->padding = SizeClassTable[idx][1];
            SizeClassTable[idx][1] = (uint64_t)scb;
        }

        // 扫描位图寻找空闲槽位
        uint64_t allocated_bit = 0xFFFFFFFFFFFFFFFFULL;
        uint64_t max_words = (scb->bit_tail / 64) + 1;

        for (uint64_t i = 0; i < max_words; i++) {
            if (scb->bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) { // 该组尚存空闲位
                uint64_t word = scb->bitmap[i];
                for (int b = 0; b < 64; b++) {
                    uint64_t current_bit = i * 64 + b;
                    if (current_bit > scb->bit_tail) break;
                    
                    if (!(word & (1ULL << b))) { // 捕捉到 0 位
                        scb->bitmap[i] |= (1ULL << b); // 置 1 分配
                        allocated_bit = current_bit;
                        break;
                    }
                }
            }
            if (allocated_bit != 0xFFFFFFFFFFFFFFFFULL) break;
        }

        // 检查分配后该块是否被彻底榨干，若是则打上全 1 满载标记
        int is_now_full = 1;
        for (uint64_t i = 0; i < max_words; i++) {
            uint64_t mask = 0xFFFFFFFFFFFFFFFFULL;
            if (i == (scb->bit_tail / 64)) {
                int remainder = (scb->bit_tail % 64) + 1;
                if (remainder < 64) mask = (1ULL << remainder) - 1;
            }
            if ((scb->bitmap[i] & mask) != mask) {
                is_now_full = 0;
                break;
            }
        }
        if (is_now_full) scb->is_full = 0xFFFFFFFFFFFFFFFFULL;

        // 计算物理载荷的虚拟首地址返回给用户
        return (void*)(scb->list_base + (allocated_bit * size_class));
    }

    // ---------------------------------------------------------------
    // 2. 大对象直通分配分支 (>= 2MB)
    // ---------------------------------------------------------------
    else {
        MainControlBlock_t* mcb = (MainControlBlock_t*)SizeClassTable[idx][1];

        // 快速跳过满载的大对象描述符管理块
        while (mcb && mcb->is_full == 1) {
            mcb = (MainControlBlock_t*)mcb->next;
        }

        // 链表为空或已满，分配全新的 16KB 大对象控制块
        if (!mcb) {
            mcb = (MainControlBlock_t*)MoreCore(4); // 16KB = 4页
            if (!mcb) return NULL;

            mcb->is_full = 0;
            for (int i = 0; i < 31; i++) mcb->bitmap[i] = 0;
            
            // 【核心安全设计】献祭最后 34 个越界位：
            // 2014 / 64 = 31 组, 2014 % 64 = 30 位。将第 31 组的第 30-63 位强制初始化为 1
            mcb->bitmap[31] = 0xFFFFFFFFFFFFFFFFULL << 30;

            // 挂载到根锚点
            mcb->next = SizeClassTable[idx][1];
            SizeClassTable[idx][1] = (uint64_t)mcb;
        }

        // 寻找 2014 个有效插槽中的空闲位
        uint64_t allocated_slot = 0xFFFFFFFFFFFFFFFFULL;
        for (int i = 0; i < 32; i++) {
            if (mcb->bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
                uint64_t word = mcb->bitmap[i];
                for (int b = 0; b < 64; b++) {
                    uint64_t slot_idx = i * 64 + b;
                    if (slot_idx >= 2014) break; // 触发献祭边界，强行终止

                    if (!(word & (1ULL << b))) {
                        mcb->bitmap[i] |= (1ULL << b);
                        allocated_slot = slot_idx;
                        break;
                    }
                }
            }
            if (allocated_slot != 0xFFFFFFFFFFFFFFFFULL) break;
        }

        // 刷新大对象控制块的满载状态
        int is_mcb_full = 1;
        for (int i = 0; i < 32; i++) {
            if (mcb->bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
                is_mcb_full = 0;
                break;
            }
        }
        if (is_mcb_full) mcb->is_full = 1;

        // 为“用户数据 + 48字节AllocBlock_t描述符”向内核申请连续的物理页
        uint64_t total_bytes_needed = size_class + sizeof(AllocBlock_t);
        uint64_t pages_to_request   = (total_bytes_needed + 4095) / 4096;
        
        void* raw_block = MoreCore(pages_to_request);
        if (!raw_block) return NULL;

        // 构造分配块描述符 (存放在大对象区域的最前端)
        AllocBlock_t* alloc_meta = (AllocBlock_t*)raw_block;
        alloc_meta->AllocSizeAligned   = size_class;
        alloc_meta->AllocSizeUnAligned = size;
        alloc_meta->BlockAddr          = (uint64_t)raw_block;
        alloc_meta->BitMapBase         = (uint64_t)mcb;
        alloc_meta->BitMapBitLocation  = allocated_slot;
        
        // 强行满足 16 字节对齐，将载荷指针推至描述符后方
        alloc_meta->AllocPtrBaseAddress = ((uint64_t)raw_block + sizeof(AllocBlock_t) + 15) & ~15ULL;

        // 将元数据描述符地址填入插槽
        mcb->list_base[allocated_slot] = (uint64_t)alloc_meta;

        return (void*)alloc_meta->AllocPtrBaseAddress;
    }
}