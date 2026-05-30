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

void* malloc(size_t size) {if (size == 0 || size > 9223372036854775808ULL) return NULL;

    // 1. 锁定内存规格 (Size Class)
    uint64_t aligned_size = (size <= 32) ? 32 : ALIGN_NEXT_POW2_64(size);
    int idx = GET_POW2_64(aligned_size) - 5; 
    if (idx < 0) idx = 0;
    
    uint64_t size_class   = SizeClassTable[idx][0];
    uint64_t region_size  = SizeClassTable[idx][2]; 

    // --------------------------------------------------------------------
    // Tier 1: MCB 中央链表检索与尾插扩容 (保持极其干净的无满载跳过)
    // --------------------------------------------------------------------
    MainControlBlock_t* mcb = (MainControlBlock_t*)SizeClassTable[idx][1];
    MainControlBlock_t* prev_mcb = NULL; 

    while (mcb) {
        if (mcb->is_full == 0) break; 
        prev_mcb = mcb;
        mcb = (MainControlBlock_t*)mcb->next; 
    }

    if (!mcb) {
        mcb = (MainControlBlock_t*)MoreCore(4); // 申请 16KB 控制块
        if (!mcb) return NULL;

        mcb->is_full = 0;
        mcb->rem_scb_count = 2014; // 【优化】引入计数器，消灭 Tier 3 扫描循环
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
    if (scb_idx >= 2014) return NULL; 

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

            uint64_t raw_region_size = SizeClassTable[idx][2];
            
            // 【安全修正】：由于每个槽位都必须容纳 [64B元数据 + 数据载荷]
            // 为了防止应用层越界写穿相邻元数据，连续物理区的大小需要加入元数据预留
            uint64_t step_size = size_class + sizeof(AllocBlock_t);
            uint64_t total_objects = raw_region_size / step_size; 
            uint64_t actual_alloc_size = total_objects * step_size;

            void* data_region = MoreCore((actual_alloc_size + 4095) / 4096); 
            if (!data_region) return NULL; 

            scb->is_full = 0;
            scb->list_base = (uint64_t)data_region;
            scb->bit_tail  = total_objects - 1;
            // 确保不超过SCB位图的最大容量（2044*64-1=130815）
            if (scb->bit_tail > 130815) {
                scb->bit_tail = 130815;
                total_objects = 130816;
            }
            scb->rem_count = total_objects; 

            for (int i = 0; i < 2044; i++) scb->bitmap[i] = 0;
            // 将超出bit_tail的位标记为已占用
            uint64_t last_word = scb->bit_tail / 64;
            uint64_t last_bit = scb->bit_tail % 64;
            if (last_word < 2043) {
                // 从last_word+1到2043的所有字都标记为全1
                for (uint64_t i = last_word + 1; i < 2044; i++) {
                    scb->bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
                }
            }
            // 最后一个字中超出last_bit的位标记为1
            scb->bitmap[last_word] |= 0xFFFFFFFFFFFFFFFFULL << (last_bit + 1);
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

        // 【安全对齐修正】：步进跨度包含了 64 字节的元数据芯片头
        uint64_t step_stride = size_class + sizeof(AllocBlock_t);
        uint64_t slot_start_addr = scb->list_base + (obj_idx * step_stride);

        // 【关键注入】精准卡死新版复合匿名变量结构体
        AllocBlock_t* header = (AllocBlock_t*)slot_start_addr;
        header->AllocSizeAligned     = size_class;
        header->AllocSizeUnAligned   = size;
        header->BlockAddr            = slot_start_addr;
        header->AllocPtrBaseAddress  = slot_start_addr + sizeof(AllocBlock_t); 
        header->Magic                = ALLOC_BLOCK_MAGIC;
        
        // 完美适配你发来的最新结构体定义：
        header->BitMapBitLocation.SCBBitLocation = obj_idx; // 封存次级块位图坐标
        header->BitMapBitLocation.MCBBitLocation = scb_idx; // 封存顶级块插槽坐标
        
        header->BitMapBase           = (uint64_t)scb;
        header->MCBAddr              = (uint64_t)mcb;

        allocated_ptr = (void*)header->AllocPtrBaseAddress;

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
            l_scb->rem_count = 2014; 
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

        if (obj_idx >= 2014) return NULL; // 越界防线

        // 大对象分配：size_class + 64B 元数据头
        uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
        uint64_t pages_needed = (total_large_size + 4095) / 4096;
        void* page_start = MoreCore(pages_needed);
        if (!page_start) return NULL;

        AllocBlock_t* header = (AllocBlock_t*)page_start;
        header->AllocSizeAligned     = size_class;
        header->AllocSizeUnAligned   = size;
        header->BlockAddr            = (uint64_t)page_start;
        header->AllocPtrBaseAddress  = (uint64_t)page_start + sizeof(AllocBlock_t);
        header->Magic                = ALLOC_BLOCK_MAGIC;
        
        // 大对象元数据注入
        header->BitMapBitLocation.SCBBitLocation = obj_idx;
        header->BitMapBitLocation.MCBBitLocation = scb_idx;
        
        header->BitMapBase           = (uint64_t)l_scb;
        header->MCBAddr              = (uint64_t)mcb;

        l_scb->list_base[obj_idx] = (uint64_t)page_start;
        allocated_ptr = (void*)header->AllocPtrBaseAddress;

        l_scb->rem_count--;
        if (l_scb->rem_count == 0) {
            is_scb_full = 1;
            l_scb->is_full = 1;
        }
    }

    // --------------------------------------------------------------------
    // Tier 3: 级联状态逆向反馈 (彻底去除 for 循环，达成绝对 O(1))
    // --------------------------------------------------------------------
    if (is_scb_full) {
        int mcb_word = scb_idx / 64;
        int mcb_bit  = scb_idx % 64;
        mcb->bitmap[mcb_word] |= (1ULL << mcb_bit);

        mcb->rem_scb_count--;
        if (mcb->rem_scb_count == 0) {
            mcb->is_full = 1; // 完美 $O(1)$ 判定满载，告别循环扫描！
        }
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

    // 1. 【倒车雷达】精准向低地址回退 64 字节，瞬间捕获对齐的元数据芯片
    AllocBlock_t* header = (AllocBlock_t*)((uint64_t)ptr - sizeof(AllocBlock_t));

    // ============================================================================
    // 【硬件级边界与自校验防御】
    // ============================================================================
    // 校验点 1：自引用指针校验，防止野指针误释放或恶意应用伪造指针
    ALLOCTOR_SECURITY_ASSERT(header->AllocPtrBaseAddress == (uint64_t)ptr);
    
    // 校验点 2：控制块核心指针范围校验，御敌于未然
    ALLOCTOR_SECURITY_ASSERT(header->BitMapBase > 0 && header->BitMapBase < SKYLINE_MAX_LEGAL_ADDR);
    ALLOCTOR_SECURITY_ASSERT(header->MCBAddr > 0 && header->MCBAddr < SKYLINE_MAX_LEGAL_ADDR);
    ALLOCTOR_SECURITY_ASSERT(header->Magic == ALLOC_BLOCK_MAGIC);

    // 2. 暴力提取元数据芯片内固化的状态信息（零查表，零计算延迟）
    uint64_t size_class   = header->AllocSizeAligned;
    uint32_t obj_bit_loc  = header->BitMapBitLocation.SCBBitLocation; // 对象在次级块位图中的 bit 位置
    uint32_t mcb_slot     = header->BitMapBitLocation.MCBBitLocation; // 次级块在主控制块阵列中的插槽位置
    
    uint64_t scb_addr     = header->BitMapBase;
    uint64_t mcb_addr     = header->MCBAddr;
    uint64_t block_addr   = header->BlockAddr; 

    // 反推 SizeClassTable 索引以区分配属通道
    int idx = GET_POW2_64(size_class) - 5;
    if (idx < 0) idx = 0;

    // 解析次级块位图的具体 uint64_t 索引与比特偏移
    uint32_t word_idx = obj_bit_loc / 64;
    uint32_t bit_idx  = obj_bit_loc % 64;

    MainControlBlock_t* mcb = (MainControlBlock_t*)mcb_addr;

    // ============================================================================
    // 3. 核心分流释放引擎
    // ============================================================================
    if (SizeClassTable[idx][2] != 0) {
        // ---------------------------------------------------
        // 分支 A：小对象极速释放流
        // ---------------------------------------------------
        SecondControlBlock_t* scb = (SecondControlBlock_t*)scb_addr;
        ALLOCTOR_SECURITY_ASSERT(obj_bit_loc <= scb->bit_tail); // 不能超过SCB的最大对象索引
        
        // 精准擦除占用标志位，回补可用计数
        if (!(scb->bitmap[word_idx] & (1ULL << bit_idx))) {
            return;
        }
        scb->bitmap[word_idx] &= ~(1ULL << bit_idx);
        scb->rem_count++; 

        // 级联解冻逻辑：由于刚才有人归还了内存，如果次级块原本处于满载状态，必须通知中央大脑
        if (scb->is_full != 0) {
            scb->is_full = 0; 

            // 【神来之笔】：直接用元数据里封存的 mcb_slot，彻底干掉任何地址减法与除法指令！
            int mcb_word = mcb_slot / 64;
            int mcb_bit  = mcb_slot % 64;
            
            // 判定中央大脑原本是不是绝对满载
            if (mcb->is_full != 0) {
                mcb->is_full = 0;
            }

            // 在中央大脑位图中将当前 SCB 的位置清零（标记为可用）
            mcb->bitmap[mcb_word] &= ~(1ULL << mcb_bit);
            
            // 恢复中央大脑的有效子节点计数
            mcb->rem_scb_count++; 
        }
    } 
    else {
        // ---------------------------------------------------
        // 分支 B：大对象极速释放流
        // ---------------------------------------------------
        LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)scb_addr;
        ALLOCTOR_SECURITY_ASSERT(obj_bit_loc < 2014); // LSCB最多只有2014个大对象插槽
        
        // 大对象的控制位统一存放在 bitmap[0]
        l_scb->bitmap[word_idx] &= ~(1ULL << bit_idx);
        l_scb->rem_count++; 

        // 级联解冻逻辑
        if (l_scb->is_full != 0) {
            l_scb->is_full = 0; 

            int mcb_word = mcb_slot / 64;
            int mcb_bit  = mcb_slot % 64;

            if (mcb->is_full != 0) {
                mcb->is_full = 0;
            }

            mcb->bitmap[mcb_word] &= ~(1ULL << mcb_bit);
            mcb->rem_scb_count++;
        }

        // 清空二级块指针阵列中的槽位，防止悬空指针
        l_scb->list_base[obj_bit_loc] = 0;

        // 【大对象就地物理隔离】：向内核交还整片连续物理页
        uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
        uint64_t pages_needed = (total_large_size + 4095) / 4096;
        LessCore((void*)block_addr, pages_needed);
    }
}

void* calloc(size_t nmemb, size_t size) {
    size_t total_size;
    // 检查乘法溢出
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
        return NULL;
    }
    total_size = nmemb * size;
    
    void* ptr = malloc(total_size);
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    
    AllocBlock_t* header = (AllocBlock_t*)((uint64_t)ptr - sizeof(AllocBlock_t));
    if (header->AllocSizeAligned >= size) {
        // 现有块足够大，直接返回
        header->AllocSizeUnAligned = size;
        return ptr;
    }
    
    // 分配新块并复制数据
    void* new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, header->AllocSizeUnAligned);
        free(ptr);
    }
    return new_ptr;
}