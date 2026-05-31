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
#ifdef __x86_64__
#include <base/arch/x86_64/atomic/common.h>
#endif



extern uint64_t SizeClassTable[75][3];
void LessCore(void* x,uint64_t y){(void)x;(void)y;return;}
void* MoreCore(uint64_t PageCount){(void)PageCount;/*Not IMPLED YET...*/return NULL;}

static inline int GetSizeClassIndex(size_t size) {
    if (size <= 32) return 0;
    uint64_t s = size - 1;
    uint32_t k = 63 - __builtin_clzll(s); 
    if (size <= 2097152ULL) { 
        uint32_t second_bit = (s >> (k - 1)) & 1;
        return ((k - 5) * 2) + 1 + second_bit;
    } else {
        return k + 12; 
    }
}



#define DIV_ROUND_UP(x, y) (((x) + ((y) - 1)) / (y))

void* malloc(size_t size) {
    if (size == 0 || size > 9223372036854775808ULL) return NULL;

    int idx = GetSizeClassIndex(size);
    if (idx < 0 || idx >= 75) return NULL; // 边界检查
    
    uint64_t size_class   = SizeClassTable[idx][0];
    uint64_t region_size  = SizeClassTable[idx][2]; 
    void* allocated_ptr   = NULL;

    // 【核心重构】：引入宏观重试循环。任何在 Tier 1/2 被别人抢占的操作，全部 continue 重试。
    while (!allocated_ptr) {
        
        // --------------------------------------------------------------------
        // Tier 1: MCB 中央链表检索与无锁尾插
        // --------------------------------------------------------------------
        MainControlBlock_t* mcb = (MainControlBlock_t*)SizeClassTable[idx][1];
        MainControlBlock_t* prev_mcb = NULL; 

        while (mcb) {
            // 脏读is_full标志，后续会有验证
            uint64_t is_full = mcb->is_full;
            if (is_full == 0) break; 
            
            // 脏读next指针
            MainControlBlock_t* next_mcb = (MainControlBlock_t*)mcb->next;
            
            if (next_mcb != NULL) {
                // 脏读rem_scb_count
                uint32_t rem_scb_count = next_mcb->rem_scb_count;
                if (rem_scb_count == 2014) {
                    // 只有当可能需要操作时，才使用原子加载验证
                    rem_scb_count = __atomic_load_n(&next_mcb->rem_scb_count, __ATOMIC_ACQUIRE);
                    if (rem_scb_count == 2014) {
                        void* cas_res = __a_cas_p((volatile void*)&mcb->next, next_mcb, (void*)next_mcb->next);
                        if (cas_res == next_mcb) {
                            LessCore(next_mcb, 4); 
                        }
                    }
                    // 链表结构变动，从头开始重试
                    continue;
                }
            }
            
            prev_mcb = mcb;
            mcb = next_mcb; 
        }

        if (!mcb) {
            MainControlBlock_t* new_mcb = (MainControlBlock_t*)MoreCore(4); 
            if (!new_mcb) return NULL;

            new_mcb->is_full = 0;
            new_mcb->rem_scb_count = 2014; 
            for (int i = 0; i < 32; i++) new_mcb->bitmap[i] = 0;
            new_mcb->bitmap[31] = 0xFFFFFFFFFFFFFFFFULL << 30; 
            for (int i = 0; i < 2014; i++) new_mcb->list_base[i] = 0;
            new_mcb->next = 0; 

            void* actual;
            if (prev_mcb == NULL) {
                actual = __a_cas_p((volatile void*)&SizeClassTable[idx][1], NULL, new_mcb);
            } else {
                actual = __a_cas_p((volatile void*)&prev_mcb->next, NULL, new_mcb);
            }
            
            if (actual != NULL) { // 竞态失败，别人建好了
                LessCore(new_mcb, 4);
                continue; // 重试，这次能拿到别人建好的 mcb
            }
            mcb = new_mcb;
        }

        // 硬件加速：扫描寻找次级插槽 (SCB Index)
        uint64_t scb_idx = 0xFFFFFFFFFFFFFFFFULL;
        for (int i = 0; i < 32; i++) {
            uint64_t current_bitmap = mcb->bitmap[i]; // 脏读即可，后续会有验证
            if (current_bitmap != 0xFFFFFFFFFFFFFFFFULL) {
                scb_idx = i * 64 + __builtin_ctzll(~current_bitmap);
                break;
            }
        }
        
        // 如果扫描不到，说明刚才的 mcb 瞬间被其他核心填满了，重试 Tier 1
        if (scb_idx >= 2014) continue; 

        // ====================================================================
        // Tier 2: 架构分流与无锁元数据注入
        // ====================================================================
        int is_scb_full = 0;

        // ---------------------------------------------------
        // 分支 A：小对象处理逻辑
        // ---------------------------------------------------
        if (region_size != 0) {
            SecondControlBlock_t* scb = (SecondControlBlock_t*)mcb->list_base[scb_idx];

            if (!scb) {
                scb = (SecondControlBlock_t*)MoreCore(4); 
                if (!scb) return NULL;

                uint64_t step_size = size_class + sizeof(AllocBlock_t);
                uint64_t total_objects = region_size / step_size; 
                void* data_region = MoreCore(((total_objects * step_size) + 4095) / 4096); 
                if (!data_region) { LessCore(scb, 4); return NULL; }

                scb->is_full = 0;
                scb->list_base = (uint64_t)data_region;
                scb->bit_tail  = total_objects - 1;
                if (scb->bit_tail > 130815) { scb->bit_tail = 130815; total_objects = 130816; }
                scb->rem_count = total_objects; 

                for (int i = 0; i < 2044; i++) scb->bitmap[i] = 0;
                uint64_t last_word = scb->bit_tail / 64;
                uint64_t last_bit = scb->bit_tail % 64;

                for (uint64_t i = last_word + 1; i < 2044; i++) {
                    scb->bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
                }

                if (last_bit < 63) {
                    scb->bitmap[last_word] |= 0xFFFFFFFFFFFFFFFFULL << (last_bit + 1);
                }
                
                // 【修改点】：原子安装 SCB。如果冲突，使用别人的并退还自己的内存。
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[scb_idx], NULL, scb);
                if (actual != NULL) {
                    LessCore(data_region, ((total_objects * step_size) + 4095) / 4096);
                    LessCore(scb, 4);
                    scb = (SecondControlBlock_t*)actual;
                }
            }

            // 【核心安全】：利用 CAS 循环抢夺 Object 槽位
            uint64_t scb_max_words = (scb->bit_tail / 64) + 1;
            uint64_t obj_idx = 0xFFFFFFFFFFFFFFFFULL;

            for (uint64_t i = 0; i < scb_max_words; i++) {
                while (1) { // 抢夺单词位图循环
                    uint64_t current_bitmap = scb->bitmap[i];
                    if (current_bitmap == 0xFFFFFFFFFFFFFFFFULL) break; // 当前字已满，看下一个
                    
                    int free_bit = __builtin_ctzll(~current_bitmap);
                    uint64_t current_bit = i * 64 + free_bit;
                    if (current_bit > scb->bit_tail) break; 
                    
                    uint64_t new_bitmap = current_bitmap | (1ULL << free_bit);
                    // 只有成功改变位图的那个线程，才算拿到了槽位
                    if (A_CAS_U64_ASM(&scb->bitmap[i], current_bitmap, new_bitmap) == current_bitmap) {
                        obj_idx = current_bit;
                        break;
                    }
                }
                if (obj_idx != 0xFFFFFFFFFFFFFFFFULL) break;
            }

            // 如果全部满载（被并发线程抢光了），回到 Tier 1 重新寻找新 SCB
            if (obj_idx == 0xFFFFFFFFFFFFFFFFULL) continue; 

            uint64_t step_stride = size_class + sizeof(AllocBlock_t);
            uint64_t slot_start_addr = scb->list_base + (obj_idx * step_stride);

            AllocBlock_t* header = (AllocBlock_t*)slot_start_addr;
            header->AllocSizeAligned     = size_class;
            header->AllocSizeUnAligned   = size;
            header->BlockAddr            = slot_start_addr;
            header->AllocPtrBaseAddress  = slot_start_addr + sizeof(AllocBlock_t); 
            header->Magic                = ALLOC_BLOCK_MAGIC;
            header->BitMapBitLocation.SCBBitLocation = obj_idx; 
            header->BitMapBitLocation.MCBBitLocation = scb_idx; 
            header->BitMapBase           = (uint64_t)scb;
            header->MCBAddr              = (uint64_t)mcb;

            allocated_ptr = (void*)header->AllocPtrBaseAddress;

            // 原子扣减余量
            uint64_t old_rem = __a_subu64(&scb->rem_count, 1); 
            if (old_rem == 1) { // 如果我是最后一个拿走内存的人
                is_scb_full = 1;
                scb->is_full = 1; // 统一使用1表示满
            }
        } 
        // ---------------------------------------------------
        // 分支 B：大对象处理逻辑
        // ---------------------------------------------------
        else {
            LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)mcb->list_base[scb_idx];

            if (!l_scb) {
                l_scb = (LargeSecondControlBlock_t*)MoreCore(4); 
                if (!l_scb) return NULL;
                
                l_scb->is_full = 0;
                l_scb->rem_count = 2014; 
                for (int i = 0; i < 32; i++) l_scb->bitmap[i] = 0;
                l_scb->bitmap[31] = 0xFFFFFFFFFFFFFFFFULL << 30; 
                for (int i = 0; i < 2014; i++) l_scb->list_base[i] = 0;
                
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[scb_idx], NULL, l_scb);
                if (actual != NULL) {
                    LessCore(l_scb, 4);
                    l_scb = (LargeSecondControlBlock_t*)actual;
                }
            }

            uint64_t obj_idx = 0xFFFFFFFFFFFFFFFFULL;
            for (int i = 0; i < 32; i++) {
                while (1) {
                    uint64_t current_bitmap = l_scb->bitmap[i];
                    if (current_bitmap == 0xFFFFFFFFFFFFFFFFULL) break;
                    
                    int free_bit = __builtin_ctzll(~current_bitmap);
                    uint64_t current_bit = i * 64 + free_bit;
                    if (current_bit >= 2014) break;

                    uint64_t new_bitmap = current_bitmap | (1ULL << free_bit);
                    if (A_CAS_U64_ASM(&l_scb->bitmap[i], current_bitmap, new_bitmap) == current_bitmap) {
                        obj_idx = current_bit;
                        break;
                    }
                }
                if (obj_idx != 0xFFFFFFFFFFFFFFFFULL) break;
            }

            if (obj_idx >= 2014) continue; // 并发满载，重试
            
            uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
            uint64_t pages_needed = (total_large_size + 4095) / 4096;
            void* page_start = MoreCore(pages_needed);
            if (!page_start) {
                uint64_t word_idx = obj_idx / 64;
                uint64_t bit_idx = obj_idx % 64;
                uint64_t mask = 1ULL << bit_idx;
                
                // 快速检查：如果位已经被清除，直接返回
                uint64_t old_val = __atomic_load_n(&l_scb->bitmap[word_idx], __ATOMIC_ACQUIRE);
                if (!(old_val & mask)) {
                    return NULL;
                }
                
                // 进入CAS循环
                uint64_t new_val;
                while (1) {
                    new_val = old_val & ~mask;
                    uint64_t actual_old = A_CAS_U64_ASM(&l_scb->bitmap[word_idx], old_val, new_val);
                    
                    if (actual_old == old_val) {
                        break;
                    }
                    
                    old_val = actual_old;
                    if (!(old_val & mask)) {
                        break;
                    }
                }
                
                return NULL;
            }

            AllocBlock_t* header = (AllocBlock_t*)page_start;
            header->AllocSizeAligned     = size_class;
            header->AllocSizeUnAligned   = size;
            header->BlockAddr            = (uint64_t)page_start;
            header->AllocPtrBaseAddress  = (uint64_t)page_start + sizeof(AllocBlock_t);
            header->Magic                = ALLOC_BLOCK_MAGIC;
            header->BitMapBitLocation.SCBBitLocation = obj_idx;
            header->BitMapBitLocation.MCBBitLocation = scb_idx;
            header->BitMapBase           = (uint64_t)l_scb;
            header->MCBAddr              = (uint64_t)mcb;

            allocated_ptr = (void*)header->AllocPtrBaseAddress;

            uint64_t old_rem = __a_subu64(&l_scb->rem_count, 1);
            if (old_rem == 1) {
                is_scb_full = 1;
                l_scb->is_full = 1;
            }
            
            // 只有在确认分配成功后，才写入list_base
            l_scb->list_base[obj_idx] = (uint64_t)page_start;
        }

        // --------------------------------------------------------------------
        // Tier 3: 级联状态逆向反馈 (原子级传播)
        // --------------------------------------------------------------------
        if (is_scb_full) {
            int mcb_word = scb_idx / 64;
            int mcb_bit  = scb_idx % 64;
            
            __a_or_64(&mcb->bitmap[mcb_word], (1ULL << mcb_bit));
            uint64_t old_mcb_rem = __a_subu32(&mcb->rem_scb_count, 1);
            if (old_mcb_rem == 1) {
                mcb->is_full = 1; 
            }
        }
    } // End of The Great Retry Loop

    return allocated_ptr;
}

#define SKYLINE_MAX_LEGAL_ADDR  0x00007FFFFFFFFFFFULL 
#define ALLOCTOR_SECURITY_ASSERT(cond) do { if (!(cond)) { return; } } while(0)

void free(void* ptr) {
    if (!ptr) return;

    AllocBlock_t* header = (AllocBlock_t*)((uint64_t)ptr - sizeof(AllocBlock_t));

    ALLOCTOR_SECURITY_ASSERT(header->AllocPtrBaseAddress == (uint64_t)ptr);
    ALLOCTOR_SECURITY_ASSERT(header->BitMapBase > 0 && header->BitMapBase < SKYLINE_MAX_LEGAL_ADDR);
    ALLOCTOR_SECURITY_ASSERT(header->MCBAddr > 0 && header->MCBAddr < SKYLINE_MAX_LEGAL_ADDR);
    ALLOCTOR_SECURITY_ASSERT(header->Magic == ALLOC_BLOCK_MAGIC);

    uint64_t size_class   = header->AllocSizeAligned;
    uint32_t obj_bit_loc  = header->BitMapBitLocation.SCBBitLocation; 
    uint32_t mcb_slot     = header->BitMapBitLocation.MCBBitLocation; 
    
    uint64_t scb_addr     = header->BitMapBase;
    uint64_t mcb_addr     = header->MCBAddr;
    uint64_t block_addr   = header->BlockAddr; 

    int idx = GetSizeClassIndex(size_class);
    if (idx < 0 || idx >= 75) return; // 边界检查
    
    uint32_t word_idx = obj_bit_loc / 64;
    uint32_t bit_idx  = obj_bit_loc % 64;

    MainControlBlock_t* mcb = (MainControlBlock_t*)mcb_addr;

    if (SizeClassTable[idx][2] != 0) {
        SecondControlBlock_t* scb = (SecondControlBlock_t*)scb_addr;
        ALLOCTOR_SECURITY_ASSERT(obj_bit_loc <= scb->bit_tail); 
        
        // 【防御双重释放】原子性地清空对象槽位
        uint64_t old_bitmap = __a_and_64(&scb->bitmap[word_idx], ~(1ULL << bit_idx));
        if (!(old_bitmap & (1ULL << bit_idx))) {
            return; // 之前已经被释放过了！
        }

        uint64_t old_rem = __a_fetch_addu64(&scb->rem_count, 1); 

        // 如果原本是全满状态，释放一个后它变为了非满，执行级联释放
        if (old_rem == 0) {
            scb->is_full = 0; 
            int mcb_word = mcb_slot / 64;
            int mcb_bit  = mcb_slot % 64;
            
            __a_and_64(&mcb->bitmap[mcb_word], ~(1ULL << mcb_bit));
            uint32_t old_mcb_rem = __a_fetch_addu32(&mcb->rem_scb_count, 1); 
            
            // 只有当MCB从完全满(rem_scb_count=0)变为有一个空闲时，才标记为非满
            if (old_mcb_rem == 0) {
                mcb->is_full = 0;
            }
        }

        // 当它变成完全空的时候 (即将从 2013 变成 2014)
        if(old_rem == 2013) {
            // 再次验证SCB是否真的完全空了
            uint64_t current_rem = __atomic_load_n(&scb->rem_count, __ATOMIC_ACQUIRE);
            if (current_rem == 2014) {
                // 利用CAS进行安全脱离
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[mcb_slot], scb, NULL);
                if (actual == scb) {
                    LessCore(scb, 4);
                }
            }
        }
        return;
    } 
    else {
        LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)scb_addr;
        ALLOCTOR_SECURITY_ASSERT(obj_bit_loc < 2014); 

        // 【极度关键】：对于大对象，必须先清理数据，最后再清理 bitmap！
        // 如果先清 bitmap，其它核心瞬间分配到了这个槽位写入新对象，你接着清零 list_base，系统直接崩盘。
        uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
        uint64_t pages_needed = (total_large_size + 4095) / 4096;
        
        l_scb->list_base[obj_bit_loc] = 0;
        LessCore((void*)block_addr, pages_needed);

        // 数据清理完毕，安全解锁槽位
        uint64_t old_bitmap = __a_and_64(&l_scb->bitmap[word_idx], ~(1ULL << bit_idx));
        if (!(old_bitmap & (1ULL << bit_idx))) return; 
        
        uint64_t old_rem = __a_fetch_addu64((volatile uint64_t*)&l_scb->rem_count, 1); 

        if (old_rem == 0) {
            l_scb->is_full = 0; 
            int mcb_word = mcb_slot / 64;
            int mcb_bit  = mcb_slot % 64;

            __a_and_64(&mcb->bitmap[mcb_word], ~(1ULL << mcb_bit));
            uint32_t old_mcb_rem = __a_fetch_addu32(&mcb->rem_scb_count, 1);
            
            // 只有当MCB从完全满变为有一个空闲时，才标记为非满
            if (old_mcb_rem == 0) {
                mcb->is_full = 0;
            }
        }

        if(old_rem == 2013) {
            // 再次验证Large SCB是否真的完全空了
            uint64_t current_rem = __atomic_load_n(&l_scb->rem_count, __ATOMIC_ACQUIRE);
            if (current_rem == 2014) {
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[mcb_slot], l_scb, NULL);
                if (actual == l_scb) {
                    LessCore(l_scb, 4);
                }
            }
        }
        return;
    }
}

// calloc 和 realloc 代码逻辑本身无锁，保持原样即可
void* calloc(size_t nmemb, size_t size) {
    size_t total_size;
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
        header->AllocSizeUnAligned = size;
        return ptr;
    }
    
    void* new_ptr = malloc(size);
    if (new_ptr) {
        memcpy(new_ptr, ptr, header->AllocSizeUnAligned);
        free(ptr);
    }
    return new_ptr;
}