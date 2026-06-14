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
/*
* SPDX-License-Identifier: GPL-2.0-only
* File: allocator.c
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*/
#include <private/alloc/alloc.h>
#include <stdc/string.h>
#ifdef __x86_64__
#include <base/arch/x86_64/atomic/common.h>
#include <base/arch/x86_64/syscall.h>
#endif
#include <stdc/stdlib.h>
#include <base/base.h>

extern uint64_t SizeClassTable[75][3];
void LessCore(void* x,uint64_t y){(void)y;
    munmap((uint64_t)x,0);
}
void* MoreCore(uint64_t PageCount){    
    void* p = (void*)mmap(0,PageCount,2,0,0);
    if(p != NULL) return p;
    else return NULL;
}

// ============================================================================
// The QSBR Subsystem & GC Queues (TLS Optimized)
// ============================================================================

#define QSBR_SLOTS 32
typedef struct {
    volatile uint64_t count;
    char padding[56]; // 强制缓存行对齐，彻底消除并发伪共享
} qsbr_slot_t;

static qsbr_slot_t qsbr_counters[QSBR_SLOTS] __attribute__((aligned(64)));

// 分代待回收队列：年轻代 + 老年代 (全局)
static volatile void* deferred_small_scb_list = NULL;  
static volatile void* deferred_large_scb_list = NULL;  
static volatile void* old_small_scb_list = NULL;       
static volatile void* old_large_scb_list = NULL;       

// GC统计与控制
static volatile uint64_t gc_generation = 0;
static volatile uint64_t pending_small_count = 0;
static volatile uint64_t pending_large_count = 0;
static volatile uint32_t gc_lock = 0;

// ----------------------------------------------------------------------------
// [TLS 核心改造区]：为每个线程分配独立的缓存队列和固定的 QSBR 槽位
// ----------------------------------------------------------------------------
static volatile uint32_t global_qsbr_slot_alloc = 0;

static __thread int32_t       tls_qsbr_slot = -1;
static __thread void* tls_small_scb_list = NULL;
static __thread uint32_t  tls_pending_small_count = 0;
static __thread void* tls_large_scb_list = NULL;
static __thread uint32_t  tls_pending_large_count = 0;

#define TLS_BATCH_FLUSH_THRESHOLD 16 // 本地队列满 16 个 SCB 时再批量推送到全局，降低 CAS 碰撞

// 批量将 TLS 小对象队列推入全局队列
static void flush_tls_small_scb() {
    if (!tls_small_scb_list) return;
    
    // 找到本地链表的尾部
    void* tail = tls_small_scb_list;
    while (*(void**)tail) {
        tail = *(void**)tail;
    }
    
    // 单次 CAS 即可将整个 TLS 链表挂载到全局队列，性能呈指数级提升
    void* old_head;
    do {
        old_head = (void*)deferred_small_scb_list;
        *(void**)tail = old_head;
        __atomic_thread_fence(__ATOMIC_RELEASE);
    } while (__a_cas_p(&deferred_small_scb_list, old_head, tls_small_scb_list) != old_head);
    
    __atomic_fetch_add(&pending_small_count, tls_pending_small_count, __ATOMIC_RELEASE);
    
    // 清空 TLS 状态
    tls_small_scb_list = NULL;
    tls_pending_small_count = 0;
}

// 批量将 TLS 大对象队列推入全局队列
static void flush_tls_large_scb() {
    if (!tls_large_scb_list) return;
    
    void* tail = tls_large_scb_list;
    while (*(void**)tail) {
        tail = *(void**)tail;
    }
    
    void* old_head;
    do {
        old_head = (void*)deferred_large_scb_list;
        *(void**)tail = old_head;
        __atomic_thread_fence(__ATOMIC_RELEASE);
    } while (__a_cas_p(&deferred_large_scb_list, old_head, tls_large_scb_list) != old_head);
    
    __atomic_fetch_add(&pending_large_count, tls_pending_large_count, __ATOMIC_RELEASE);
    
    tls_large_scb_list = NULL;
    tls_pending_large_count = 0;
}

// 供外部调用：当线程被销毁前，务必调用此函数清空 TLS 缓存
void allocator_thread_exit_cleanup() {
    flush_tls_small_scb();
    flush_tls_large_scb();
}

static void push_deferred_small_scb(void* scb) {
    // 纯 TLS 无锁操作
    *(void**)scb = tls_small_scb_list;
    tls_small_scb_list = scb;
    tls_pending_small_count++;
    
    if (tls_pending_small_count >= TLS_BATCH_FLUSH_THRESHOLD) {
        flush_tls_small_scb();
    }
}

static void push_deferred_large_scb(void* scb) {
    // 纯 TLS 无锁操作
    *(void**)scb = tls_large_scb_list;
    tls_large_scb_list = scb;
    tls_pending_large_count++;
    
    if (tls_pending_large_count >= TLS_BATCH_FLUSH_THRESHOLD) {
        flush_tls_large_scb();
    }
}

static inline int32_t qsbr_enter() {
    // [TLS优化]：取代原先昂贵的 rdtsc() 运算
    if (__builtin_expect(tls_qsbr_slot == -1, 0)) {
        tls_qsbr_slot = __atomic_fetch_add(&global_qsbr_slot_alloc, 1, __ATOMIC_RELAXED) % QSBR_SLOTS;
    }
    __atomic_fetch_add(&qsbr_counters[tls_qsbr_slot].count, 1, __ATOMIC_ACQUIRE);
    return tls_qsbr_slot;
}

static inline void qsbr_leave(int32_t slot) {
    __atomic_sub_fetch(&qsbr_counters[slot].count, 1, __ATOMIC_RELEASE);
}

// 核心优化：静止期检测替代全静止检测
static inline int32_t is_quiescent() {
    uint64_t snapshot[QSBR_SLOTS];
    uint64_t generation = gc_generation;
    
    for (int32_t i = 0; i < QSBR_SLOTS; i++) {
        snapshot[i] = __atomic_load_n(&qsbr_counters[i].count, __ATOMIC_ACQUIRE);
    }
    
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    
    for (int32_t retry = 0; retry < 1000; retry++) {
        int32_t all_advanced = 1;
        
        for (int32_t i = 0; i < QSBR_SLOTS; i++) {
            uint64_t current = __atomic_load_n(&qsbr_counters[i].count, __ATOMIC_ACQUIRE);
            if (current <= snapshot[i]) {
                all_advanced = 0;
                break;
            }
        }
        
        if (all_advanced) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&gc_generation, __ATOMIC_ACQUIRE) == generation) {
                return 1;
            }
            return 0;
        }
        
        for (int32_t i = 0; i < 1000; i++) {
            CPU_RELAX();
        }
    }
    return 0;
}

// 优化版GC：多轮重试 + 压力触发 + 分代队列
static inline void try_gc() {
    // 尝试 GC 前，先把当前线程积压的 TLS 缓存推入全局，防止漏收
    flush_tls_small_scb();
    flush_tls_large_scb();

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
    
    uint64_t total_pending = pending_small_count + pending_large_count;
    int32_t max_retries = 10;
    int32_t pause_count = 1000;
    
    if (total_pending > 100) {
        max_retries = 50;
        pause_count = 5000;
    }
    if (total_pending > 1000) {
        max_retries = 200;
        pause_count = 20000;
    }
    
    int32_t gc_success = 0;
    for (int32_t retry = 0; retry < max_retries; retry++) {
        if (is_quiescent()) {
            gc_success = 1;
            break;
        }
        for (int32_t i = 0; i < pause_count; i++) {
            CPU_RELAX();
        }
    }
    
    if (gc_success) {
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
        
        while (large_list) {
            void* next = *(void**)large_list;
            LessCore(large_list, 4);
            large_list = next;
        }
        
        __atomic_store_n(&pending_small_count, 0, __ATOMIC_RELEASE);
        __atomic_store_n(&pending_large_count, 0, __ATOMIC_RELEASE);
        __atomic_fetch_add(&gc_generation, 1, __ATOMIC_RELEASE);
    } else {
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

static inline int32_t GetSizeClassIndex(size_t size) {
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
// 内部逻辑：实际的分配与释放引擎 (无更改，保持优秀架构)
// ============================================================================
static void* _skyline_malloc_internal(size_t size) {
    if (size == 0 || size > 9223372036854775808ULL) return NULL;

    int32_t idx = GetSizeClassIndex(size);
    if (idx < 0 || idx >= 75) return NULL;
    
    uint64_t size_class   = SizeClassTable[idx][0];
    uint64_t region_size  = SizeClassTable[idx][2]; 
    void* allocated_ptr   = NULL;

    while (!allocated_ptr) {
        MainControlBlock_t* mcb = NULL;
        MainControlBlock_t* prev_mcb = NULL;
        
        for (int32_t retry = 0; retry < 10; retry++) {
            mcb = (MainControlBlock_t*)SizeClassTable[idx][1];
            prev_mcb = NULL;
            
            while (mcb) {
                __builtin_prefetch(mcb, 0, 3);
                
                if (__atomic_load_n(&mcb->is_full, __ATOMIC_ACQUIRE) == 0) {
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
            for (int32_t i = 0; i < 32; i++) new_mcb->bitmap[i] = 0;
            new_mcb->bitmap[31] = 0xFFFFFFFFFFFFFFFFULL << 30; 
            for (int32_t i = 0; i < 2014; i++) new_mcb->list_base[i] = 0;
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
        for (int32_t i = 0; i < 32; i++) {
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
            __builtin_prefetch(scb, 0, 3); 

            if (!scb) {
                uint64_t step_size = size_class + sizeof(AllocBlock_t);
                uint64_t total_objects = region_size / step_size; 
                if (total_objects > 130816) total_objects = 130816;

                void* expected = NULL;
                if (__atomic_compare_exchange_n(&mcb->list_base[scb_idx], &expected, (void*)1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    scb = (SecondControlBlock_t*)MoreCore(4); 
                    if (!scb) {
                        __atomic_store_n(&mcb->list_base[scb_idx], NULL, __ATOMIC_RELEASE);
                        CPU_RELAX();
                        continue; 
                    }

                    void* data_region = MoreCore(((total_objects * step_size) + 4095) / 4096); 
                    if (!data_region) { 
                        LessCore(scb, 4);
                        __atomic_store_n(&mcb->list_base[scb_idx], NULL, __ATOMIC_RELEASE);
                        CPU_RELAX();
                        continue; 
                    }

                    scb->is_full = 0;
                    scb->list_base = (uint64_t)data_region;
                    scb->bit_tail  = total_objects - 1;
                    scb->rem_count = total_objects;
                    scb->step_size = step_size;

                    for (int32_t i = 0; i < 2044; i++) scb->bitmap[i] = 0;
                    uint64_t last_word = scb->bit_tail / 64;
                    uint64_t last_bit = scb->bit_tail % 64;

                    for (uint64_t i = last_word + 1; i < 2044; i++) {
                        scb->bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
                    }
                    if (last_bit < 63) {
                        scb->bitmap[last_word] |= 0xFFFFFFFFFFFFFFFFULL << (last_bit + 1);
                    }
                    
                    __atomic_store_n(&mcb->list_base[scb_idx], scb, __ATOMIC_RELEASE);
                } else {
                    while ((scb = (SecondControlBlock_t*)__atomic_load_n(&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE)) == (void*)1) {
                        CPU_RELAX();
                    }
                    if (!scb) {
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
                    
                    int32_t free_bit = __builtin_ctzll(~current_bitmap);
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
                if (__atomic_compare_exchange_n(&scb->is_full, &(uint64_t){0}, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    int32_t mcb_word = scb_idx / 64;
                    int32_t mcb_bit  = scb_idx % 64;
                    __a_or_64(&mcb->bitmap[mcb_word], (1ULL << mcb_bit));
                    uint32_t old_mcb_rem = __a_subu32(&mcb->rem_scb_count, 1);
                    if (old_mcb_rem == 1) mcb->is_full = 1;
                }
            }
        } 
        else {
            LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)mcb->list_base[scb_idx];

            if (!l_scb) {
                void* expected = NULL;
                if (__atomic_compare_exchange_n(&mcb->list_base[scb_idx], &expected, (void*)1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    l_scb = (LargeSecondControlBlock_t*)MoreCore(4); 
                    if (!l_scb) {
                        __atomic_store_n(&mcb->list_base[scb_idx], NULL, __ATOMIC_RELEASE);
                        CPU_RELAX();
                        continue; 
                    }
                    
                    l_scb->is_full = 0;
                    l_scb->rem_count = 2014; 
                    for (int32_t i = 0; i < 32; i++) l_scb->bitmap[i] = 0;
                    l_scb->bitmap[31] = 0xFFFFFFFFFFFFFFFFULL << 30; 
                    for (int32_t i = 0; i < 2014; i++) l_scb->list_base[i] = 0;
                    
                    __atomic_store_n(&mcb->list_base[scb_idx], l_scb, __ATOMIC_RELEASE);
                } else {
                    while ((l_scb = (LargeSecondControlBlock_t*)__atomic_load_n(&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE)) == (void*)1) {
                        CPU_RELAX();
                    }
                    if (!l_scb) {
                        CPU_RELAX();
                        continue;
                    }
                }
            }

            uint64_t obj_idx = 0xFFFFFFFFFFFFFFFFULL;
            for (int32_t i = 0; i < 32; i++) {
                while (1) {
                    uint64_t current_bitmap = l_scb->bitmap[i];
                    if (current_bitmap == 0xFFFFFFFFFFFFFFFFULL) break;
                    
                    int32_t free_bit = __builtin_ctzll(~current_bitmap);
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

            uint64_t old_rem = __a_subu64(&l_scb->rem_count, 1);
            if (old_rem == 1) {
                uint64_t expected = 0;
                if (__atomic_compare_exchange_n(&l_scb->is_full, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    int32_t mcb_word = scb_idx / 64;
                    int32_t mcb_bit  = scb_idx % 64;
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

    int32_t idx = GetSizeClassIndex(size_class);
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
            int32_t mcb_word = mcb_slot / 64;
            int32_t mcb_bit  = mcb_slot % 64;
            
            __a_clear_bit(&mcb->bitmap[mcb_word], (1ULL << mcb_bit));
            uint32_t old_mcb_rem = __a_fetch_addu32(&mcb->rem_scb_count, 1); 
            if (old_mcb_rem == 0) mcb->is_full = 0;
        }

        uint64_t target_rem = scb->bit_tail + 1;
        if (old_rem == target_rem - 1) {
            uint64_t current_rem = __atomic_load_n(&scb->rem_count, __ATOMIC_ACQUIRE);
            if (current_rem == target_rem) {
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[mcb_slot], scb, NULL);
                if (actual == scb) {
                    push_deferred_small_scb(scb); // 直接压入极速的 TLS 队列
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
            int32_t mcb_word = mcb_slot / 64;
            int32_t mcb_bit  = mcb_slot % 64;

            __a_clear_bit(&mcb->bitmap[mcb_word], (1ULL << mcb_bit));
            uint32_t old_mcb_rem = __a_fetch_addu32(&mcb->rem_scb_count, 1);
            if (old_mcb_rem == 0) mcb->is_full = 0;
        }

        if (old_rem == 2013) {
            uint64_t current_rem = __atomic_load_n(&l_scb->rem_count, __ATOMIC_ACQUIRE);
            if (current_rem == 2014) {
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[mcb_slot], l_scb, NULL);
                if (actual == l_scb) {
                    push_deferred_large_scb(l_scb); // 直接压入极速的 TLS 队列
                }
            }
        }
        return;
    }
}

// ============================================================================
// 对外公开的 Standard API 封装层
// ============================================================================

void* malloc(size_t size) {
    int32_t qsbr_slot = qsbr_enter();
    void* ptr = _skyline_malloc_internal(size);
    qsbr_leave(qsbr_slot);
    return ptr;
}

void free(void* ptr) {
    if (!ptr) return;
    int32_t qsbr_slot = qsbr_enter();
    _skyline_free_internal(ptr);
    qsbr_leave(qsbr_slot);
    
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
        int32_t new_idx = GetSizeClassIndex(size);
        int32_t old_idx = GetSizeClassIndex(header->AllocSizeAligned);
        if ((new_idx < old_idx && header->AllocSizeAligned - size > 4096)
        || (SizeClassTable[old_idx][2] == 0 && new_idx != old_idx)) {
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