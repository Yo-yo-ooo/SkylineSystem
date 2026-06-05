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

//The QSBR Subsystem

#define QSBR_SLOTS 32
typedef struct {
    volatile uint64_t count;
    char padding[56]; // 强制缓存行对齐，彻底消除并发伪共享
} qsbr_slot_t;

static qsbr_slot_t qsbr_counters[QSBR_SLOTS] __attribute__((aligned(64)));

// 分代待回收队列：年轻代 + 老年代
static volatile void* deferred_small_scb_list = NULL;  // 小对象SCB年轻代
static volatile void* deferred_large_scb_list = NULL;  // 大对象SCB年轻代
static volatile void* old_small_scb_list = NULL;       // 小对象SCB老年代
static volatile void* old_large_scb_list = NULL;       // 大对象SCB老年代

// GC统计与控制
static volatile uint64_t gc_generation = 0;
static volatile uint64_t pending_small_count = 0;
static volatile uint64_t pending_large_count = 0;
static volatile uint32_t gc_lock = 0;

// 专用push函数，带计数
// 在push_deferred_small_scb和push_deferred_large_scb函数中添加内存屏障
static void push_deferred_small_scb(void* scb) {
    void* old_head;
    do {
        old_head = (void*)deferred_small_scb_list;
        *(void**)scb = old_head;
        // 确保scb的内容在发布之前对所有线程可见
        __atomic_thread_fence(__ATOMIC_RELEASE);
    } while (__a_cas_p(&deferred_small_scb_list, old_head, scb) != old_head);
    
    __atomic_fetch_add(&pending_small_count, 1, __ATOMIC_RELEASE);
}

static void push_deferred_large_scb(void* scb) {
    void* old_head;
    do {
        old_head = (void*)deferred_large_scb_list;
        *(void**)scb = old_head;
        // 确保scb的内容在发布之前对所有线程可见
        __atomic_thread_fence(__ATOMIC_RELEASE);
    } while (__a_cas_p(&deferred_large_scb_list, old_head, scb) != old_head);
    
    __atomic_fetch_add(&pending_large_count, 1, __ATOMIC_RELEASE);
}

static inline int qsbr_enter() {
    uint32_t hash = (uint32_t)(__builtin_ia32_rdtsc() % QSBR_SLOTS);
    __atomic_fetch_add(&qsbr_counters[hash].count, 1, __ATOMIC_ACQUIRE);
    return hash;
}

static inline void qsbr_leave(int hash) {
    __atomic_sub_fetch(&qsbr_counters[hash].count, 1, __ATOMIC_RELEASE);
}

// 核心优化：静止期检测替代全静止检测
// 不再要求所有CPU同时静止，只要求所有CPU都经历过至少一次静止
// 替换原有的is_quiescent函数
static inline int is_quiescent() {
    uint64_t snapshot[QSBR_SLOTS];
    uint64_t generation = gc_generation;
    
    // 第一步：获取当前世代的计数器快照
    for (int i = 0; i < QSBR_SLOTS; i++) {
        snapshot[i] = __atomic_load_n(&qsbr_counters[i].count, __ATOMIC_ACQUIRE);
    }
    
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    
    // 第二步：等待所有槽位的计数器都至少增加了1
    // 证明每个槽位都至少有一个线程进入并离开过临界区
    for (int retry = 0; retry < 1000; retry++) {
        int all_advanced = 1;
        
        for (int i = 0; i < QSBR_SLOTS; i++) {
            uint64_t current = __atomic_load_n(&qsbr_counters[i].count, __ATOMIC_ACQUIRE);
            if (current <= snapshot[i]) {
                all_advanced = 0;
                break;
            }
        }
        
        if (all_advanced) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            // 确保GC世代没有变化
            if (__atomic_load_n(&gc_generation, __ATOMIC_ACQUIRE) == generation) {
                return 1;
            }
            return 0;
        }
        
        for (int i = 0; i < 1000; i++) {
            CPU_RELAX();
        }
    }
    
    return 0;
}

// 优化版GC：多轮重试 + 压力触发 + 分代队列
static inline void try_gc() {
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gc_lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return;
    }
    
    // 先剥离老年代队列
    void* old_small = (void*)(uintptr_t)__atomic_exchange_n(&old_small_scb_list, NULL, __ATOMIC_ACQ_REL);
    void* old_large = (void*)(uintptr_t)__atomic_exchange_n(&old_large_scb_list, NULL, __ATOMIC_ACQ_REL);
    
    // 再剥离年轻代队列
    void* young_small = (void*)(uintptr_t)__atomic_exchange_n(&deferred_small_scb_list, NULL, __ATOMIC_ACQ_REL);
    void* young_large = (void*)(uintptr_t)__atomic_exchange_n(&deferred_large_scb_list, NULL, __ATOMIC_ACQ_REL);
    
    // 合并队列：老年代在前，年轻代在后
    void* small_list = old_small;
    if (young_small) {
        void* tail = young_small;
        while (*(void**)tail) {
            tail = *(void**)tail;
        }
        *(void**)tail = small_list;
        small_list = young_small;
    }
    
    void* large_list = old_large;
    if (young_large) {
        void* tail = young_large;
        while (*(void**)tail) {
            tail = *(void**)tail;
        }
        *(void**)tail = large_list;
        large_list = young_large;
    }
    
    if (!small_list && !large_list) {
        goto unlock;
    }
    
    // 根据待回收数量动态调整重试次数和等待时间
    uint64_t total_pending = pending_small_count + pending_large_count;
    int max_retries = 10;
    int pause_count = 1000;
    
    if (total_pending > 100) {
        max_retries = 50;
        pause_count = 5000;
    }
    if (total_pending > 1000) {
        max_retries = 200;
        pause_count = 20000;
    }
    
    // 多轮重试静止期检测
    int gc_success = 0;
    for (int retry = 0; retry < max_retries; retry++) {
        if (is_quiescent()) {
            gc_success = 1;
            break;
        }
        
        // 短暂等待，给其他线程时间离开临界区
        for (int i = 0; i < pause_count; i++) {
            CPU_RELAX();
        }
    }
    
    if (gc_success) {
        // 回收小对象SCB：释放数据区 + 释放SCB本身
        while (small_list) {
            void* next = *(void**)small_list;
            SecondControlBlock_t* scb = (SecondControlBlock_t*)small_list;
            
            uint64_t total_objects = scb->bit_tail + 1;
            uint64_t total_size = total_objects * scb->step_size;
            uint64_t pages_needed = (total_size + 4095) / 4096;
            LessCore((void*)scb->list_base, pages_needed);
            
            LessCore(small_list, 4);
            small_list = next;
        }
        
        // 回收大对象SCB：只释放SCB本身
        while (large_list) {
            void* next = *(void**)large_list;
            LessCore(large_list, 4);
            large_list = next;
        }
        
        // 重置计数器并推进GC世代
        __atomic_store_n(&pending_small_count, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&pending_large_count, 0, __ATOMIC_RELEASE);
        __atomic_fetch_add(&gc_generation, 1, __ATOMIC_RELEASE);
    } else {
        // GC失败：将本次未回收的块晋升到老年代
        if (small_list) {
            void* tail = small_list;
            while (*(void**)tail) {
                tail = *(void**)tail;
            }
            void* old_head;
            do {
                old_head = (void*)old_small_scb_list;
                *(void**)tail = old_head;
            } while (__a_cas_p(&old_small_scb_list, old_head, small_list) != old_head);
        }
        
        if (large_list) {
            void* tail = large_list;
            while (*(void**)tail) {
                tail = *(void**)tail;
            }
            void* old_head;
            do {
                old_head = (void*)old_large_scb_list;
                *(void**)tail = old_head;
            } while (__a_cas_p(&old_large_scb_list, old_head, large_list) != old_head);
        }
    }
    
unlock:
    __atomic_store_n(&gc_lock, 0, __ATOMIC_RELEASE);
}

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

// ============================================================================
// 内部逻辑：实际的分配与释放引擎（完全保留原有优秀架构）
// ============================================================================
static void* _skyline_malloc_internal(size_t size) {
    if (size == 0 || size > 9223372036854775808ULL) return NULL;

    int idx = GetSizeClassIndex(size);
    if (idx < 0 || idx >= 75) return NULL;
    
    uint64_t size_class   = SizeClassTable[idx][0];
    uint64_t region_size  = SizeClassTable[idx][2]; 
    void* allocated_ptr   = NULL;

    while (!allocated_ptr) {
        MainControlBlock_t* mcb = NULL;
        MainControlBlock_t* prev_mcb = NULL;
        
        // 重试直到找到一个有效的非满MCB
        for (int retry = 0; retry < 10; retry++) {
            mcb = (MainControlBlock_t*)SizeClassTable[idx][1];
            prev_mcb = NULL;
            
            while (mcb) {
                __builtin_prefetch(mcb, 0, 3);
                
                // 先检查is_full，再获取next指针
                if (__atomic_load_n(&mcb->is_full, __ATOMIC_ACQUIRE) == 0) {
                    // 确认这个MCB仍然在链表中
                    if (prev_mcb == NULL || __atomic_load_n(&prev_mcb->next, __ATOMIC_ACQUIRE) == (uint64_t)mcb) {
                        break;
                    }
                }
                
                prev_mcb = mcb;
                mcb = (MainControlBlock_t*)__atomic_load_n(&mcb->next, __ATOMIC_ACQUIRE);
            }
            
            if (mcb) break;
            CPU_RELAX();
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
            
            if (actual != NULL) { 
                LessCore(new_mcb, 4);
                CPU_RELAX();
                continue; 
            }
            mcb = new_mcb;
        }

        uint64_t scb_idx = 0xFFFFFFFFFFFFFFFFULL;
        for (int i = 0; i < 32; i++) {
            uint64_t current_bitmap = mcb->bitmap[i]; 
            if (current_bitmap != 0xFFFFFFFFFFFFFFFFULL) {
                scb_idx = i * 64 + __builtin_ctzll(~current_bitmap);
                break;
            }
        }
        
        if (scb_idx >= 2014) {
            CPU_RELAX(); 
            continue; 
        }

        if (region_size != 0) {
            SecondControlBlock_t* scb = (SecondControlBlock_t*)mcb->list_base[scb_idx];
            __builtin_prefetch(scb, 0, 3); // 预取MCB数据

            // 在_skyline_malloc_internal函数中替换SCB创建逻辑
            if (!scb) {
                uint64_t step_size = size_class + sizeof(AllocBlock_t);
                uint64_t total_objects = region_size / step_size; 
                if (total_objects > 130816) total_objects = 130816;

                // 先尝试CAS抢占创建权
                void* expected = NULL;
                if (__atomic_compare_exchange_n(&mcb->list_base[scb_idx], &expected, (void*)1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    // 我们获得了创建权
                    scb = (SecondControlBlock_t*)MoreCore(4); 
                    if (!scb) {
                        __atomic_store_n(&mcb->list_base[scb_idx], NULL, __ATOMIC_RELEASE);
                        CPU_RELAX();
                        continue; // 重试整个分配流程，而不是直接返回失败
                    }

                    void* data_region = MoreCore(((total_objects * step_size) + 4095) / 4096); 
                    if (!data_region) { 
                        LessCore(scb, 4);
                        __atomic_store_n(&mcb->list_base[scb_idx], NULL, __ATOMIC_RELEASE);
                        CPU_RELAX();
                        continue; // 重试整个分配流程
                    }

                    scb->is_full = 0;
                    scb->list_base = (uint64_t)data_region;
                    scb->bit_tail  = total_objects - 1;
                    scb->rem_count = total_objects;
                    scb->step_size = step_size;

                    for (int i = 0; i < 2044; i++) scb->bitmap[i] = 0;
                    uint64_t last_word = scb->bit_tail / 64;
                    uint64_t last_bit = scb->bit_tail % 64;

                    for (uint64_t i = last_word + 1; i < 2044; i++) {
                        scb->bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
                    }
                    if (last_bit < 63) {
                        scb->bitmap[last_word] |= 0xFFFFFFFFFFFFFFFFULL << (last_bit + 1);
                    }
                    
                    // 发布SCB
                    __atomic_store_n(&mcb->list_base[scb_idx], scb, __ATOMIC_RELEASE);
                } else {
                    // 等待其他线程完成创建
                    while ((scb = (SecondControlBlock_t*)__atomic_load_n(&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE)) == (void*)1) {
                        CPU_RELAX();
                    }
                    if (!scb) {
                        // 创建失败，重试整个分配流程
                        CPU_RELAX();
                        continue;
                    }
                }
            }

            uint64_t scb_max_words = (scb->bit_tail / 64) + 1;
            uint64_t obj_idx = 0xFFFFFFFFFFFFFFFFULL;

            for (uint64_t i = 0; i < scb_max_words; i++) {
                while (1) { 
                    uint64_t current_bitmap = scb->bitmap[i];
                    if (current_bitmap == 0xFFFFFFFFFFFFFFFFULL) break; 
                    
                    int free_bit = __builtin_ctzll(~current_bitmap);
                    uint64_t current_bit = i * 64 + free_bit;
                    if (current_bit > scb->bit_tail) break; 
                    
                    uint64_t new_bitmap = current_bitmap | (1ULL << free_bit);
                    if (A_CAS_U64_ASM(&scb->bitmap[i], current_bitmap, new_bitmap) == current_bitmap) {
                        if (__atomic_load_n((uint64_t*)&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE) != (uint64_t)scb) {
                            uint64_t rb_old, rb_new;
                            do { 
                                rb_old = scb->bitmap[i];
                                rb_new = rb_old & ~(1ULL << free_bit);
                            } while (A_CAS_U64_ASM(&scb->bitmap[i], rb_old, rb_new) != rb_old);
                            // 修复：脱轨检测触发后，同步拉高控制变量，彻底刺穿终止外层 for 循环扫描
                            i = scb_max_words; 
                            break; 
                        }
                        obj_idx = current_bit;
                        break;
                    }
                    CPU_RELAX(); 
                }
                if (obj_idx != 0xFFFFFFFFFFFFFFFFULL) break;
            }

            if (obj_idx == 0xFFFFFFFFFFFFFFFFULL) {
                CPU_RELAX(); 
                continue; 
            }

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

            uint64_t old_rem = __a_subu32(&scb->rem_count, 1); 
            if (old_rem == 1) { 
                // 原子设置is_full为1，只有当它原来为0时才成功
                if (__atomic_compare_exchange_n(&scb->is_full, &(uint64_t){0}, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    int mcb_word = scb_idx / 64;
                    int mcb_bit  = scb_idx % 64;
                    __a_or_64(&mcb->bitmap[mcb_word], (1ULL << mcb_bit));
                    uint32_t old_mcb_rem = __a_subu32(&mcb->rem_scb_count, 1);
                    if (old_mcb_rem == 1) mcb->is_full = 1;
                }
            }
        } 
        else {
            LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)mcb->list_base[scb_idx];

            // 在_skyline_malloc_internal函数中替换大对象SCB创建逻辑
            if (!l_scb) {
                // 先尝试CAS抢占创建权
                void* expected = NULL;
                if (__atomic_compare_exchange_n(&mcb->list_base[scb_idx], &expected, (void*)1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    // 我们获得了创建权
                    l_scb = (LargeSecondControlBlock_t*)MoreCore(4); 
                    if (!l_scb) {
                        __atomic_store_n(&mcb->list_base[scb_idx], NULL, __ATOMIC_RELEASE);
                        CPU_RELAX();
                        continue; // 重试整个分配流程，而不是直接返回失败
                    }
                    
                    l_scb->is_full = 0;
                    l_scb->rem_count = 2014; 
                    for (int i = 0; i < 32; i++) l_scb->bitmap[i] = 0;
                    l_scb->bitmap[31] = 0xFFFFFFFFFFFFFFFFULL << 30; 
                    for (int i = 0; i < 2014; i++) l_scb->list_base[i] = 0;
                    
                    // 发布SCB
                    __atomic_store_n(&mcb->list_base[scb_idx], l_scb, __ATOMIC_RELEASE);
                } else {
                    // 等待其他线程完成创建
                    while ((l_scb = (LargeSecondControlBlock_t*)__atomic_load_n(&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE)) == (void*)1) {
                        CPU_RELAX();
                    }
                    if (!l_scb) {
                        // 创建失败，重试整个分配流程
                        CPU_RELAX();
                        continue;
                    }
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
                        if (__atomic_load_n((uint64_t*)&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE) != (uint64_t)l_scb) {
                            uint64_t rb_old, rb_new;
                            do {
                                rb_old = l_scb->bitmap[i];
                                rb_new = rb_old & ~(1ULL << free_bit);
                            } while (A_CAS_U64_ASM(&l_scb->bitmap[i], rb_old, rb_new) != rb_old);
                            // 修复：大对象脱轨刺穿
                            i = 32; 
                            break;
                        }
                        obj_idx = current_bit;
                        break;
                    }
                    CPU_RELAX(); 
                }
                if (obj_idx != 0xFFFFFFFFFFFFFFFFULL) break;
            }

            if (obj_idx >= 2014) {
                CPU_RELAX();
                continue; 
            }
            
            uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
            uint64_t pages_needed = (total_large_size + 4095) / 4096;
            void* page_start = MoreCore(pages_needed);
            if (!page_start) {
                uint64_t word_idx = obj_idx / 64;
                uint64_t bit_idx = obj_idx % 64;
                uint64_t mask = 1ULL << bit_idx;
                
                // 先验证l_scb仍然有效
                if (__atomic_load_n(&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE) == (uint64_t)l_scb) {
                    uint64_t old_val = __atomic_load_n(&l_scb->bitmap[word_idx], __ATOMIC_ACQUIRE);
                    if (old_val & mask) {
                        __a_clear_bit(&l_scb->bitmap[word_idx], mask);
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
            l_scb->list_base[obj_idx] = (uint64_t)page_start;

            // 大对象分配逻辑
            uint64_t old_rem = __a_subu64(&l_scb->rem_count, 1);
            if (old_rem == 1) {
                // 原子设置is_full为1，只有当它原来为0时才成功
                uint64_t expected = 0;
                if (__atomic_compare_exchange_n(&l_scb->is_full, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    int mcb_word = scb_idx / 64;
                    int mcb_bit  = scb_idx % 64;
                    __a_or_64(&mcb->bitmap[mcb_word], (1ULL << mcb_bit));
                    uint32_t old_mcb_rem = __a_subu32(&mcb->rem_scb_count, 1);
                    if (old_mcb_rem == 1) mcb->is_full = 1;
                }
            }
        }
    } 
    return allocated_ptr;
}

#define SKYLINE_MAX_LEGAL_ADDR  0x00007FFFFFFFFFFFULL 
#define ALLOCTOR_SECURITY_ASSERT(cond) do { if (!(cond)) { return; } } while(0)
static void _skyline_free_internal(void* ptr) {
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
    if (idx < 0 || idx >= 75) return; 
    
    uint32_t word_idx = obj_bit_loc / 64;
    uint32_t bit_idx  = obj_bit_loc % 64;

    MainControlBlock_t* mcb = (MainControlBlock_t*)mcb_addr;

    if (SizeClassTable[idx][2] != 0) {
        SecondControlBlock_t* scb = (SecondControlBlock_t*)scb_addr;
        ALLOCTOR_SECURITY_ASSERT(obj_bit_loc <= scb->bit_tail); 
        
        uint64_t old_bitmap = __a_clear_bit(&scb->bitmap[word_idx], (1ULL << bit_idx));
        if (!(old_bitmap & (1ULL << bit_idx))) return; 

        uint64_t old_rem = __a_fetch_addu32(&scb->rem_count, 1); 

        if (old_rem == 0) {
            scb->is_full = 0; 
            int mcb_word = mcb_slot / 64;
            int mcb_bit  = mcb_slot % 64;
            
            __a_clear_bit(&mcb->bitmap[mcb_word], (1ULL << mcb_bit));
            uint32_t old_mcb_rem = __a_fetch_addu32(&mcb->rem_scb_count, 1); 
            if (old_mcb_rem == 0) mcb->is_full = 0;
        }

        // 根据 scb->bit_tail 动态推导并定义完全清空的阈值 target_rem
        uint64_t target_rem = scb->bit_tail + 1;
        // 小对象分支
        if (old_rem == target_rem - 1) {
            uint64_t current_rem = __atomic_load_n(&scb->rem_count, __ATOMIC_ACQUIRE);
            if (current_rem == target_rem) {
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[mcb_slot], scb, NULL);
                if (actual == scb) {
                    push_deferred_small_scb(scb);  // 压入小对象队列
                }
            }
        }
        return;
    } 
    else {
        LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)scb_addr;
        ALLOCTOR_SECURITY_ASSERT(obj_bit_loc < 2014); 

        uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
        uint64_t pages_needed = (total_large_size + 4095) / 4096;
        
        l_scb->list_base[obj_bit_loc] = 0;
        LessCore((void*)block_addr, pages_needed); 

        uint64_t old_bitmap = __a_clear_bit(&l_scb->bitmap[word_idx], (1ULL << bit_idx));
        if (!(old_bitmap & (1ULL << bit_idx))) return; 
        
        uint64_t old_rem = __a_fetch_addu64((volatile uint64_t*)&l_scb->rem_count, 1); 

        if (old_rem == 0) {
            l_scb->is_full = 0; 
            int mcb_word = mcb_slot / 64;
            int mcb_bit  = mcb_slot % 64;

            __a_clear_bit(&mcb->bitmap[mcb_word], (1ULL << mcb_bit));
            uint32_t old_mcb_rem = __a_fetch_addu32(&mcb->rem_scb_count, 1);
            if (old_mcb_rem == 0) mcb->is_full = 0;
        }

        // 大对象分支
        if (old_rem == 2013) {
            uint64_t current_rem = __atomic_load_n(&l_scb->rem_count, __ATOMIC_ACQUIRE);
            if (current_rem == 2014) {
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[mcb_slot], l_scb, NULL);
                if (actual == l_scb) {
                    push_deferred_large_scb(l_scb);  // 压入大对象队列
                }
            }
        }
        return;
    }
}

// ============================================================================
// 对外公开的 Standard API 封装层 (嵌入 QSBR 保护期)
// ============================================================================

void* malloc(size_t size) {
    int qsbr_slot = qsbr_enter();
    void* ptr = _skyline_malloc_internal(size);
    qsbr_leave(qsbr_slot);
    return ptr;
}

void free(void* ptr) {
    if (!ptr) return;
    int qsbr_slot = qsbr_enter();
    _skyline_free_internal(ptr);
    qsbr_leave(qsbr_slot);
    
    // 脱离无锁结构访问期后，顺手尝试进行垃圾清理
    // 这让物理内存能够在系统并发压力降维时立刻回流 OS
    try_gc(); 
}

void* calloc(size_t nmemb, size_t size) {
    size_t total_size;
    if (nmemb != 0 && size > SIZE_MAX / nmemb) return NULL;
    total_size = nmemb * size;
    
    void* ptr = malloc(total_size);
    if (ptr) memset(ptr, 0, total_size);
    return ptr;
}

void* realloc(void* ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    
    AllocBlock_t* header = (AllocBlock_t*)((uint64_t)ptr - sizeof(AllocBlock_t));
    if (header->AllocSizeAligned >= size) {
        // 如果缩小到另一个大小类，考虑重新分配
        int new_idx = GetSizeClassIndex(size);
        int old_idx = GetSizeClassIndex(header->AllocSizeAligned);
        if ((new_idx < old_idx && header->AllocSizeAligned - size > 4096)
        || (SizeClassTable[old_idx][2] == 0 && new_idx != old_idx)) {
            // 节省的内存超过一页，重新分配
            void* new_ptr = malloc(size);
            if (new_ptr) {
                memcpy(new_ptr, ptr, size);
                free(ptr);
                return new_ptr;
            }
        }
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