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

void* malloc(size_t size) {
    if (size == 0 || size > 9223372036854775808ULL) return NULL;

    // 1. 锁定内存规格 (Size Class)
    uint64_t aligned_size = ALIGN_NEXT_POW2_64(size);
    int idx = GET_POW2_64(aligned_size) - 5; 
    uint64_t size_class   = SizeClassTable[idx][0];
    uint64_t region_size  = SizeClassTable[idx][2]; 

    // --------------------------------------------------------------------
    // Tier 1: MCB 中央链表检索与尾插扩容
    // --------------------------------------------------------------------
    MainControlBlock_t* mcb = (MainControlBlock_t*)SizeClassTable[idx][1];
    MainControlBlock_t* prev_mcb = NULL; 

    while (mcb) {
        if (mcb->is_full == 0) break; 
        prev_mcb = mcb;
        mcb = (MainControlBlock_t*)mcb->next; 
    }

    if (!mcb) {
        mcb = (MainControlBlock_t*)MoreCore(4); 
        if (!mcb) return NULL;

        mcb->is_full = 0;
        for (int i = 0; i < 32; i++) mcb->bitmap[i] = 0;
        mcb->bitmap[31] = 0xFFFFFFFFFFFFFFFFULL << 30; // 锁死 2014~2047 槽位
        for (int i = 0; i < 2014; i++) mcb->list_base[i] = 0;
        mcb->next = 0; 

        if (SizeClassTable[idx][1] == 0) {
            SizeClassTable[idx][1] = (uint64_t)mcb;
        } else {
            prev_mcb->next = (uint64_t)mcb;
        }
    }

    // 硬件加速：扫描 MCB 位图寻找空闲次级插槽 (SCB Index)
    uint64_t scb_idx = 0xFFFFFFFFFFFFFFFFULL;
    for (int i = 0; i < 32; i++) {
        if (mcb->bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
            scb_idx = i * 64 + __builtin_ctzll(~mcb->bitmap[i]);
            break;
        }
    }
    if (scb_idx >= 2014) return NULL; // 内核安全防线

    // ====================================================================
    // Tier 2: 架构分流与元数据芯片（AllocBlock）注入
    // ====================================================================
    void* allocated_ptr = NULL;
    int is_scb_full = 0;

    // ---------------------------------------------------
    // 分支 A：小对象处理逻辑 (RegionSize != 0)
    // ---------------------------------------------------
    if (region_size != 0) {
        SecondControlBlock_t* scb = (SecondControlBlock_t*)mcb->list_base[scb_idx];

        if (!scb) {
            scb = (SecondControlBlock_t*)MoreCore(4); 
            if (!scb) return NULL;

            // 严格遵循修正：从 SizeClassTable[idx][2] 获取精确连续物理区大小
            uint64_t raw_region_size = SizeClassTable[idx][2];
            void* data_region = MoreCore(raw_region_size / 4096); 
            if (!data_region) return NULL; 

            scb->is_full = 0;
            scb->list_base = (uint64_t)data_region;
            
            uint64_t total_objects = raw_region_size / size_class;
            scb->bit_tail  = total_objects - 1;
            scb->rem_count = total_objects; // 初始化可用计数

            for (int i = 0; i < 2044; i++) scb->bitmap[i] = 0;
            mcb->list_base[scb_idx] = (uint64_t)scb;
        }

        // 定位具体的 Object 内存槽位
        uint64_t scb_max_words = (scb->bit_tail / 64) + 1;
        uint64_t obj_idx = 0xFFFFFFFFFFFFFFFFULL;

        for (uint64_t i = 0; i < scb_max_words; i++) {
            if (scb->bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
                int free_bit = __builtin_ctzll(~scb->bitmap[i]);
                uint64_t current_bit = i * 64 + free_bit;
                
                if (current_bit <= scb->bit_tail) {
                    scb->bitmap[i] |= (1ULL << free_bit);
                    obj_idx = current_bit;
                    break;
                }
            }
        }

        if (obj_idx == 0xFFFFFFFFFFFFFFFFULL) return NULL; 

        // 计算当前对象的绝对物理起始基地址
        uint64_t slot_start_addr = scb->list_base + (obj_idx * size_class);

        // 【关键注入】在其头部构建反向查找元数据
        AllocBlock_t* header = (AllocBlock_t*)slot_start_addr;
        header->AllocSizeAligned    = size_class;
        header->AllocSizeUnAligned  = size;
        header->BlockAddr           = slot_start_addr;
        header->AllocPtrBaseAddress = slot_start_addr + sizeof(AllocBlock_t); // 越过元数据后的数据区
        header->BitMapBitLocation   = obj_idx;
        header->BitMapBase          = (uint64_t)scb;
        header->MCBAddr             = (uint64_t)mcb;

        // 最终返回用户可见的数据区指针
        allocated_ptr = (void*)header->AllocPtrBaseAddress;

        // O(1) 极速满载判定
        scb->rem_count--; 
        if (scb->rem_count == 0) {
            is_scb_full = 1;
            scb->is_full = 0xFFFFFFFFFFFFFFFFULL; 
        }
    } 
    // ---------------------------------------------------
    // 分支 B：大对象处理逻辑 (RegionSize == 0)
    // ---------------------------------------------------
    else {
        LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)mcb->list_base[scb_idx];

        if (!l_scb) {
            l_scb = (LargeSecondControlBlock_t*)MoreCore(4); 
            if (!l_scb) return NULL;
            
            l_scb->is_full = 0;
            l_scb->rem_count = 2014; // 大对象二级块一共拥有 2014 个离散插槽
            for (int i = 0; i < 32; i++) l_scb->bitmap[i] = 0;
            l_scb->bitmap[31] = 0xFFFFFFFFFFFFFFFFULL << 30; // 锁死 2014~2047
            for (int i = 0; i < 2014; i++) l_scb->list_base[i] = 0;
            
            mcb->list_base[scb_idx] = (uint64_t)l_scb;
        }

        uint64_t obj_idx = 0xFFFFFFFFFFFFFFFFULL;
        for (int i = 0; i < 32; i++) {
            if (l_scb->bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
                int free_bit = __builtin_ctzll(~l_scb->bitmap[i]);
                obj_idx = i * 64 + free_bit;
                l_scb->bitmap[i] |= (1ULL << free_bit); 
                break;
            }
        }

        if (obj_idx == 0xFFFFFFFFFFFFFFFFULL) return NULL;

        // 大对象分配：除了用户规格所需空间，必须额外加上 AllocBlock_t 的开销
        uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
        uint64_t pages_needed = (total_large_size + 4095) / 4096;
        void* page_start = MoreCore(pages_needed);
        if (!page_start) return NULL;

        // 【关键注入】大对象独立的连续物理页开头同样填入反向元数据
        AllocBlock_t* header = (AllocBlock_t*)page_start;
        header->AllocSizeAligned    = size_class;
        header->AllocSizeUnAligned  = size;
        header->BlockAddr           = (uint64_t)page_start;
        header->AllocPtrBaseAddress = (uint64_t)page_start + sizeof(AllocBlock_t);
        header->BitMapBitLocation   = obj_idx;
        header->BitMapBase          = (uint64_t)l_scb;
        header->MCBAddr             = (uint64_t)mcb;

        // 登记到指针插槽中
        l_scb->list_base[obj_idx] = (uint64_t)page_start;
        allocated_ptr = (void*)header->AllocPtrBaseAddress;

        // 大对象满载判定优化为 O(1)
        l_scb->rem_count--;
        if (l_scb->rem_count == 0) {
            is_scb_full = 1;
            l_scb->is_full = 1;
        }
    }

    // --------------------------------------------------------------------
    // Tier 3: 级联状态逆向反馈
    // --------------------------------------------------------------------
    if (is_scb_full) {
        int mcb_word = scb_idx / 64;
        int mcb_bit  = scb_idx % 64;
        mcb->bitmap[mcb_word] |= (1ULL << mcb_bit);

        int is_mcb_full = 1;
        for (int i = 0; i < 32; i++) {
            if (mcb->bitmap[i] != 0xFFFFFFFFFFFFFFFFULL) {
                is_mcb_full = 0;
                break;
            }
        }
        if (is_mcb_full) mcb->is_full = 1;
    }

    return allocated_ptr;
}


void LessCore(void* x,uint64_t y){(void)x;(void)y;return;}

#define SKYLINE_MAX_LEGAL_ADDR  0x00007FFFFFFFFFFFULL 

// 安全断言宏：一旦条件不满足，立刻触发内核 Panic，御敌于国门之外
#define ALLOCTOR_SECURITY_ASSERT(cond) \
    do { if (!(cond)) { return; } } while(0)

void free(void* ptr) {
    if (!ptr) return;

    // 1. 【倒车雷达】向低地址移动 64 字节，捕获硬件对齐元数据
    AllocBlock_t* header = (AllocBlock_t*)((uint64_t)ptr - sizeof(AllocBlock_t));

    // 安全检查：指针自引用校验
    ALLOCTOR_SECURITY_ASSERT(header->AllocPtrBaseAddress == (uint64_t)ptr);
    ALLOCTOR_SECURITY_ASSERT(header->BitMapBase > 0 && header->BitMapBase < SKYLINE_MAX_LEGAL_ADDR);
    ALLOCTOR_SECURITY_ASSERT(header->MCBAddr > 0 && header->MCBAddr < SKYLINE_MAX_LEGAL_ADDR);

    uint64_t size_class   = header->AllocSizeAligned;
    uint64_t bit_location = header->BitMapBitLocation;
    uint64_t block_addr   = header->BlockAddr; 
    uint64_t scb_addr     = header->BitMapBase;
    uint64_t mcb_addr     = header->MCBAddr;

    // 定位全局表索引与位图格子
    int idx = GET_POW2_64(size_class) - 5;
    uint64_t word_idx = bit_location / 64;
    uint64_t bit_idx  = bit_location % 64;

    MainControlBlock_t* mcb = (MainControlBlock_t*)mcb_addr;

    // 2. 区分大小对象规格，进入强类型极速释放流程
    if (SizeClassTable[idx][2] != 0) {
        // =================【小对象释放流】=================
        SecondControlBlock_t* scb = (SecondControlBlock_t*)scb_addr;
        
        // 清除占用标志
        scb->bitmap[word_idx] &= ~(1ULL << bit_idx);
        scb->rem_count++; 

        // 级联解冻：利用 header 带来的全局通道，实现无循环 O(1) 通报
        if (scb->is_full != 0) {
            scb->is_full = 0; 

            // 通过计算当前 scb 地址在 mcb 列表中的偏移，直接反推槽位 (slot)
            // 彻底消灭任何查找，无锁无循环
            uint64_t slot = ((uint64_t)scb - (uint64_t)&(mcb->list_base[0])) / sizeof(uint64_t);
            ALLOCTOR_SECURITY_ASSERT(slot < 2014); // 边界防御

            mcb->bitmap[slot / 64] &= ~(1ULL << (slot % 64));
            mcb->is_full = 0;
        }
    } 
    else {
        // =================【大对象释放流】=================
        LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)scb_addr;
        
        l_scb->bitmap[word_idx] &= ~(1ULL << bit_idx);
        l_scb->rem_count++; 

        // 级联解冻
        if (l_scb->is_full != 0) {
            l_scb->is_full = 0; 

            // 地址算术直接计算槽位
            uint64_t slot = ((uint64_t)l_scb - (uint64_t)&(mcb->list_base[0])) / sizeof(uint64_t);
            ALLOCTOR_SECURITY_ASSERT(slot < 2014);

            mcb->bitmap[slot / 64] &= ~(1ULL << (slot % 64));
            mcb->is_full = 0;
        }

        // 物理大页就地向系统割离（包含 64 字节的 Header 空间）
        uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
        uint64_t pages_needed = (total_large_size + 4095) / 4096;
        LessCore((void*)block_addr, pages_needed);
    }
}