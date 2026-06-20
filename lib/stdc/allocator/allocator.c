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
#include <base/arch/x86_64/syscall.h>
#endif
#include <stdc/stdlib.h>
#include <base/base.h>

extern uint64_t SizeClassTable[75][3];

// ============================================================================
// 常量宏定义
// ============================================================================
#define QSBR_SLOTS              32
#define TLS_BATCH_FLUSH_THRESHOLD 16
#define SCT_MAX_CLASSES         75
#define SCT_MAX_ALLOC_SIZE      9223372036854775808ULL

#define MCB_SCB_COUNT           2014    
#define MCB_BITMAP_WORDS        32      
#define SCB_MAX_OBJECTS         130816  
#define SCB_BITMAP_WORDS        2044    

// [FIX] 重试次数放大，避免高并发下假性 OOM
#define ALLOC_MAX_RETRIES       10000     
#define SCB_INIT_SPIN_TIMEOUT   10000   

#define LARGE_OBJ_CACHE_MAX     4       

#define DIV_ROUND_UP(x, y) (((x) + ((y) - 1)) / (y))
#define PAGE_SIZE 4096

// [FIX] 补充后备的 CPU_RELAX 防止未定义报错
#ifndef CPU_RELAX
#if defined(__x86_64__) || defined(__i386__)
#define CPU_RELAX() __asm__ __volatile__("pause\n": : :"memory")
#elif defined(__aarch64__)
#define CPU_RELAX() __asm__ __volatile__("yield\n": : :"memory")
#else
#define CPU_RELAX() do { } while(0)
#endif
#endif

void LessCore(void* x, uint64_t y) {
    (void)y;
    sys_munmap((uint64_t)x, 0); // 补全系统调用参数
}

void* MoreCore(uint64_t PageCount) {
    void* p = (void*)sys_mmap(0, PageCount, 2, 0, 0);
    return (p != NULL) ? p : NULL;
}

// ============================================================================
// QSBR 子系统 & GC 分代队列
// ============================================================================

typedef struct {
    volatile uint64_t count;
    char padding[56]; 
} qsbr_slot_t;

static qsbr_slot_t qsbr_counters[QSBR_SLOTS] __attribute__((aligned(64)));

static volatile void* deferred_small_scb_list = NULL;
static volatile void* deferred_large_scb_list = NULL;
static volatile void* old_small_scb_list = NULL;
static volatile void* old_large_scb_list = NULL;

static volatile uint64_t gc_generation = 0;
static volatile uint64_t pending_small_count = 0;
static volatile uint64_t pending_large_count = 0;
static volatile uint32_t gc_lock = 0;
static volatile uint32_t global_qsbr_slot_alloc = 0;

typedef struct {
    int32_t  qsbr_slot;
    uint32_t pending_small;
    uint32_t pending_large;
    void* small_list;
    void* large_list;
} tls_alloc_data_t;

static __thread tls_alloc_data_t tls_data = { -1, 0, 0, NULL, NULL };

static __thread void* tls_large_cache[SCT_MAX_CLASSES] = {NULL};
static __thread uint32_t tls_large_cache_cnt[SCT_MAX_CLASSES] = {0};

static void try_gc();
static void _free_large_object_real(void* block_addr, AllocBlock_t* header);

// ============================================================================
// TLS 与全局队列无锁交互
// ============================================================================

#define FLUSH_TLS_LIST(tls_list, tls_count, global_list, global_count) do { \
    if (!(tls_list)) break; \
    void* tail = (tls_list); \
    while (*(void**)tail) tail = *(void**)tail; \
    void* old_head; \
    do { \
        old_head = (void*)(global_list); \
        *(void**)tail = old_head; \
        __atomic_thread_fence(__ATOMIC_RELEASE); \
    } while (__a_cas_p(&(global_list), old_head, (tls_list)) != old_head); \
    __atomic_fetch_add(&(global_count), (tls_count), __ATOMIC_RELEASE); \
    (tls_list) = NULL; \
    (tls_count) = 0; \
} while(0)

static void flush_tls_scb_all() {
    FLUSH_TLS_LIST(tls_data.small_list, tls_data.pending_small, deferred_small_scb_list, pending_small_count);
    FLUSH_TLS_LIST(tls_data.large_list, tls_data.pending_large, deferred_large_scb_list, pending_large_count);
}

void allocator_thread_exit_cleanup() {
    flush_tls_scb_all();
    for (int32_t i = 0; i < SCT_MAX_CLASSES; i++) {
        void* p = tls_large_cache[i];
        while (p) {
            void* next = *(void**)p;
            // [FIX] 不能直接 LessCore，否则 LSCB 槽位永久丢失导致泄漏
            // 改为调用底层的脱离 Cache 的 large free 方法
            AllocBlock_t* header = (AllocBlock_t*)p;
            _free_large_object_real(p, header);
            p = next;
        }
        tls_large_cache[i] = NULL;
        tls_large_cache_cnt[i] = 0;
    }
    // [FIX] 刚刚释放 LSCB 可能会推入新的 GC 队列，必须再 Flush 一次
    flush_tls_scb_all(); 
}

static inline void push_deferred_scb(void* scb, int is_large) {
    if (!is_large) {
        *(void**)scb = tls_data.small_list;
        tls_data.small_list = scb;
        if (++tls_data.pending_small >= TLS_BATCH_FLUSH_THRESHOLD) {
            FLUSH_TLS_LIST(tls_data.small_list, tls_data.pending_small, deferred_small_scb_list, pending_small_count);
            try_gc();
        }
    } else {
        *(void**)scb = tls_data.large_list;
        tls_data.large_list = scb;
        if (++tls_data.pending_large >= TLS_BATCH_FLUSH_THRESHOLD) {
            FLUSH_TLS_LIST(tls_data.large_list, tls_data.pending_large, deferred_large_scb_list, pending_large_count);
            try_gc();
        }
    }
}

// ============================================================================
// QSBR 与 GC 核心
// ============================================================================

static inline int32_t qsbr_enter() {
    if (__builtin_expect(tls_data.qsbr_slot == -1, 0)) {
        tls_data.qsbr_slot = __atomic_fetch_add(&global_qsbr_slot_alloc, 1, __ATOMIC_RELAXED) % QSBR_SLOTS;
    }
    __atomic_fetch_add(&qsbr_counters[tls_data.qsbr_slot].count, 1, __ATOMIC_ACQUIRE);
    return tls_data.qsbr_slot;
}

static inline void qsbr_leave(int32_t slot) {
    __atomic_sub_fetch(&qsbr_counters[slot].count, 1, __ATOMIC_RELEASE);
}

static inline int32_t is_quiescent() {
    uint64_t snapshot[QSBR_SLOTS];
    uint64_t generation = gc_generation;

    for (int32_t i = 0; i < QSBR_SLOTS; i++) {
        snapshot[i] = __atomic_load_n(&qsbr_counters[i].count, __ATOMIC_ACQUIRE);
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    for (int32_t retry = 0; retry < 50; retry++) {
        int32_t all_advanced = 1;
        for (int32_t i = 0; i < QSBR_SLOTS; i++) {
            if (__atomic_load_n(&qsbr_counters[i].count, __ATOMIC_ACQUIRE) <= snapshot[i]) {
                all_advanced = 0;
                break;
            }
        }
        if (all_advanced) {
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            return (__atomic_load_n(&gc_generation, __ATOMIC_ACQUIRE) == generation);
        }
        for (int32_t i = 0; i < 50; i++) CPU_RELAX();
    }
    return 0;
}

static inline void try_gc() {
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gc_lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return;
    }

    flush_tls_scb_all();

    void* small_list = (void*)(uintptr_t)__atomic_exchange_n(&old_small_scb_list, NULL, __ATOMIC_ACQ_REL);
    void* large_list = (void*)(uintptr_t)__atomic_exchange_n(&old_large_scb_list, NULL, __ATOMIC_ACQ_REL);
    void* young_small = (void*)(uintptr_t)__atomic_exchange_n(&deferred_small_scb_list, NULL, __ATOMIC_ACQ_REL);
    void* young_large = (void*)(uintptr_t)__atomic_exchange_n(&deferred_large_scb_list, NULL, __ATOMIC_ACQ_REL);

    #define APPEND_LIST(head, tail_list) do { \
        if (tail_list) { \
            void* t = (tail_list); \
            while (*(void**)t) t = *(void**)t; \
            *(void**)t = (head); \
            (head) = (tail_list); \
        } \
    } while(0)

    APPEND_LIST(small_list, young_small);
    APPEND_LIST(large_list, young_large);

    if (!small_list && !large_list) goto unlock;

    int32_t gc_success = is_quiescent();

    if (gc_success) {
        while (small_list) {
            void* next = *(void**)small_list;
            SecondControlBlock_t* scb = (SecondControlBlock_t*)small_list;
            uint64_t pages_needed = (((scb->bit_tail + 1) * scb->step_size) + 4095) / 4096;
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
        void* old_head;
        if (small_list) {
            void* tail = small_list;
            while (*(void**)tail) tail = *(void**)tail;
            do {
                old_head = (void*)old_small_scb_list;
                *(void**)tail = old_head;
            } while (__a_cas_p(&old_small_scb_list, old_head, small_list) != old_head);
        }
        if (large_list) {
            void* tail = large_list;
            while (*(void**)tail) tail = *(void**)tail;
            do {
                old_head = (void*)old_large_scb_list;
                *(void**)tail = old_head;
            } while (__a_cas_p(&old_large_scb_list, old_head, large_list) != old_head);
        }
    }

unlock:
    __atomic_store_n(&gc_lock, 0, __ATOMIC_RELEASE);
}

// ============================================================================
// SizeClass 索引计算
// ============================================================================

int GetSizeClassIndex(uint64_t size) {
    if (size == 0) size = 32ULL;
    if (size > SCT_MAX_ALLOC_SIZE) return -1;

    int left = 0, right = SCT_MAX_CLASSES - 1;
    int ans = -1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (SizeClassTable[mid][0] >= size) {
            ans = mid;
            right = mid - 1;
        } else {
            left = mid + 1;
        }
    }
    return ans;
}

// ============================================================================
// 分配器核心
// ============================================================================

static void* _skyline_malloc_internal(size_t size) {
    if (size == 0 || size > SCT_MAX_ALLOC_SIZE) return NULL;

    int32_t idx = GetSizeClassIndex(size);
    if (idx < 0 || idx >= SCT_MAX_CLASSES) return NULL;

    uint64_t size_class  = SizeClassTable[idx][0];
    uint64_t region_size = SizeClassTable[idx][2];
    void* allocated_ptr  = NULL;

    int32_t alloc_retries = 0;

    if (region_size == 0 && tls_large_cache_cnt[idx] > 0) {
        AllocBlock_t* header = (AllocBlock_t*)tls_large_cache[idx];
        tls_large_cache[idx] = *(void**)header;
        tls_large_cache_cnt[idx]--;
        header->AllocSizeUnAligned = size;
        return (void*)header->AllocPtrBaseAddress;
    }

    while (!allocated_ptr) {
        if (++alloc_retries > ALLOC_MAX_RETRIES) {
            try_gc();
            return NULL;
        }

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
            if (!new_mcb) { try_gc(); return NULL; }

            new_mcb->is_full = 0;
            new_mcb->rem_scb_count = MCB_SCB_COUNT;
            for (int32_t i = 0; i < MCB_BITMAP_WORDS; i++) new_mcb->bitmap[i] = 0;
            new_mcb->bitmap[MCB_BITMAP_WORDS - 1] = 0xFFFFFFFFFFFFFFFFULL << 30;
            for (int32_t i = 0; i < MCB_SCB_COUNT; i++) new_mcb->list_base[i] = 0;
            new_mcb->next = 0;

            void* actual = (prev_mcb == NULL)
                ? __a_cas_p((volatile void*)&SizeClassTable[idx][1], NULL, new_mcb)
                : __a_cas_p((volatile void*)&prev_mcb->next, NULL, new_mcb);

            if (actual != NULL) {
                LessCore(new_mcb, 4);
                continue;
            }
            mcb = new_mcb;
        }

        uint64_t scb_idx = 0xFFFFFFFFFFFFFFFFULL;
        for (int32_t i = 0; i < MCB_BITMAP_WORDS; i++) {
            uint64_t current_bitmap = mcb->bitmap[i];
            if (current_bitmap != 0xFFFFFFFFFFFFFFFFULL) {
                scb_idx = (uint64_t)i * 64 + __builtin_ctzll(~current_bitmap);
                break;
            }
        }

        if (scb_idx >= MCB_SCB_COUNT) {
            mcb->is_full = 1;
            continue;
        }

        if (region_size != 0) {
            // ==================== 小对象路径 ====================
            SecondControlBlock_t* scb = (SecondControlBlock_t*)mcb->list_base[scb_idx];
            __builtin_prefetch(scb, 0, 3);

            if (!scb) {
                uint64_t step_size = size_class + sizeof(AllocBlock_t);
                uint64_t total_objects = region_size / step_size;
                if (total_objects > SCB_MAX_OBJECTS) total_objects = SCB_MAX_OBJECTS;

                void* expected = NULL;
                if (__atomic_compare_exchange_n(&mcb->list_base[scb_idx], &expected, (void*)1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    scb = (SecondControlBlock_t*)MoreCore(4);
                    if (!scb) {
                        __atomic_store_n(&mcb->list_base[scb_idx], NULL, __ATOMIC_RELEASE);
                        try_gc();
                        continue;
                    }

                    void* data_region = MoreCore(((total_objects * step_size) + 4095) / 4096);
                    if (!data_region) {
                        LessCore(scb, 4);
                        __atomic_store_n(&mcb->list_base[scb_idx], NULL, __ATOMIC_RELEASE);
                        try_gc();
                        continue;
                    }

                    scb->is_full = 0;
                    scb->list_base = (uint64_t)data_region;
                    scb->bit_tail  = total_objects - 1;
                    scb->rem_count = total_objects;
                    scb->step_size = step_size;

                    for (int32_t i = 0; i < SCB_BITMAP_WORDS; i++) scb->bitmap[i] = 0;
                    uint64_t last_word = scb->bit_tail / 64;
                    uint64_t last_bit = scb->bit_tail % 64;

                    for (uint64_t i = last_word + 1; i < SCB_BITMAP_WORDS; i++) {
                        scb->bitmap[i] = 0xFFFFFFFFFFFFFFFFULL;
                    }
                    if (last_bit < 63) {
                        scb->bitmap[last_word] |= 0xFFFFFFFFFFFFFFFFULL << (last_bit + 1);
                    }

                    __atomic_store_n(&mcb->list_base[scb_idx], scb, __ATOMIC_RELEASE);
                } else {
                    int32_t spin_cnt = 0;
                    while ((scb = (SecondControlBlock_t*)__atomic_load_n(&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE)) == (void*)1) {
                        if (++spin_cnt >= SCB_INIT_SPIN_TIMEOUT) {
                            break; // [FIX] 超时仅放弃获取，绝不能重置为 NULL，否则破坏占位者状态
                        }
                        CPU_RELAX();
                    }
                    if (scb == (void*)1 || !scb) continue; // [FIX] 被占用或依然为空则跳过该块
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
                // [FIX] SCB其实已满(滞后未同步到MCB)。手动更新MCB位图消除该块，防止高频空转死循环
                uint64_t mask = 1ULL << (scb_idx % 64);
                uint64_t old_val = __atomic_fetch_or(&mcb->bitmap[scb_idx / 64], mask, __ATOMIC_RELAXED);
                if (!(old_val & mask)) {
                    if (__a_subu32(&mcb->rem_scb_count, 1) == 1) {
                        __atomic_store_n(&mcb->is_full, 1, __ATOMIC_RELEASE);
                    }
                }
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

            if (__a_subu32(&scb->rem_count, 1) == 1) {
                if (__atomic_compare_exchange_n(&scb->is_full, &(uint64_t){0}, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    __a_or_64(&mcb->bitmap[scb_idx / 64], (1ULL << (scb_idx % 64)));
                    if (__a_subu32(&mcb->rem_scb_count, 1) == 1) mcb->is_full = 1;
                }
            }
        }
        else {
            // ==================== 大对象路径 ====================
            LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)mcb->list_base[scb_idx];

            if (!l_scb) {
                void* expected = NULL;
                if (__atomic_compare_exchange_n(&mcb->list_base[scb_idx], &expected, (void*)1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    l_scb = (LargeSecondControlBlock_t*)MoreCore(4);
                    if (!l_scb) {
                        __atomic_store_n(&mcb->list_base[scb_idx], NULL, __ATOMIC_RELEASE);
                        try_gc();
                        continue;
                    }

                    l_scb->is_full = 0;
                    l_scb->rem_count = MCB_SCB_COUNT;
                    for (int32_t i = 0; i < MCB_BITMAP_WORDS; i++) l_scb->bitmap[i] = 0;
                    l_scb->bitmap[MCB_BITMAP_WORDS - 1] = 0xFFFFFFFFFFFFFFFFULL << 30;
                    for (int32_t i = 0; i < MCB_SCB_COUNT; i++) l_scb->list_base[i] = 0;

                    __atomic_store_n(&mcb->list_base[scb_idx], l_scb, __ATOMIC_RELEASE);
                } else {
                    int32_t spin_cnt = 0;
                    while ((l_scb = (LargeSecondControlBlock_t*)__atomic_load_n(&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE)) == (void*)1) {
                        if (++spin_cnt >= SCB_INIT_SPIN_TIMEOUT) {
                            break; // [FIX] 同上，超时不重置，直接抛弃本次竞争
                        }
                        CPU_RELAX();
                    }
                    if (l_scb == (void*)1 || !l_scb) continue; // [FIX] 保护机制
                }
            }

            uint64_t obj_idx = 0xFFFFFFFFFFFFFFFFULL;
            for (int32_t i = 0; i < MCB_BITMAP_WORDS; i++) {
                while (1) {
                    uint64_t current_bitmap = l_scb->bitmap[i];
                    if (current_bitmap == 0xFFFFFFFFFFFFFFFFULL) break;

                    int32_t free_bit = __builtin_ctzll(~current_bitmap);
                    uint64_t current_bit = (uint64_t)i * 64 + free_bit;
                    if (current_bit >= MCB_SCB_COUNT) break;

                    uint64_t new_bitmap = current_bitmap | (1ULL << free_bit);
                    if (A_CAS_U64_ASM(&l_scb->bitmap[i], current_bitmap, new_bitmap) == current_bitmap) {
                        if (__atomic_load_n((uint64_t*)&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE) != (uint64_t)l_scb) {
                            uint64_t rb_old, rb_new;
                            do {
                                rb_old = l_scb->bitmap[i];
                                rb_new = rb_old & ~(1ULL << free_bit);
                            } while (A_CAS_U64_ASM(&l_scb->bitmap[i], rb_old, rb_new) != rb_old);
                            i = MCB_BITMAP_WORDS;
                            break;
                        }
                        obj_idx = current_bit;
                        break;
                    }
                    CPU_RELAX();
                }
                if (obj_idx != 0xFFFFFFFFFFFFFFFFULL) break;
            }

            if (obj_idx >= MCB_SCB_COUNT) {
                // [FIX] 消除滞后的 LSCB 位图信息引发的空转死循环
                uint64_t mask = 1ULL << (scb_idx % 64);
                uint64_t old_val = __atomic_fetch_or(&mcb->bitmap[scb_idx / 64], mask, __ATOMIC_RELAXED);
                if (!(old_val & mask)) {
                    if (__a_subu32(&mcb->rem_scb_count, 1) == 1) {
                        __atomic_store_n(&mcb->is_full, 1, __ATOMIC_RELEASE);
                    }
                }
                continue;
            }

            uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
            void* page_start = MoreCore((total_large_size + 4095) / 4096);
            if (!page_start) {
                uint64_t mask = 1ULL << (obj_idx % 64);
                if (__atomic_load_n(&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE) == (uint64_t)l_scb) {
                    if (__atomic_load_n(&l_scb->bitmap[obj_idx / 64], __ATOMIC_ACQUIRE) & mask) {
                        __a_clear_bit(&l_scb->bitmap[obj_idx / 64], mask);
                    }
                }
                try_gc();
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

            if (__a_subu64(&l_scb->rem_count, 1) == 1) {
                uint64_t expected = 0;
                if (__atomic_compare_exchange_n(&l_scb->is_full, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
                    __a_or_64(&mcb->bitmap[scb_idx / 64], (1ULL << (scb_idx % 64)));
                    if (__a_subu32(&mcb->rem_scb_count, 1) == 1) mcb->is_full = 1;
                }
            }
        }
    }
    return allocated_ptr;
}

// ============================================================================
// 释放路径
// ============================================================================

#define SKYLINE_MAX_LEGAL_ADDR  0x00007FFFFFFFFFFFULL
#define ALLOCTOR_SECURITY_ASSERT(cond) do { if (!(cond)) { return; } } while(0)

// [FIX] 抽出大对象的底层释放逻辑，用于在线程退出清空 TLS Cache 时正确回收 LSCB 槽位
static void _free_large_object_real(void* block_addr, AllocBlock_t* header) {
    uint64_t size_class  = header->AllocSizeAligned;
    uint32_t obj_bit_loc = header->BitMapBitLocation.SCBBitLocation;
    uint32_t mcb_slot    = header->BitMapBitLocation.MCBBitLocation;
    uint64_t scb_addr    = header->BitMapBase;
    uint64_t mcb_addr    = header->MCBAddr;

    LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)scb_addr;
    MainControlBlock_t* mcb = (MainControlBlock_t*)mcb_addr;
    uint32_t word_idx = obj_bit_loc / 64;
    uint32_t bit_idx  = obj_bit_loc % 64;

    ALLOCTOR_SECURITY_ASSERT(obj_bit_loc < MCB_SCB_COUNT);

    l_scb->list_base[obj_bit_loc] = 0;
    LessCore((void*)block_addr, (size_class + sizeof(AllocBlock_t) + 4095) / 4096);

    if (!(__a_clear_bit(&l_scb->bitmap[word_idx], (1ULL << bit_idx)) & (1ULL << bit_idx))) return;

    uint64_t old_rem = __a_fetch_addu64((volatile uint64_t*)&l_scb->rem_count, 1);
    if (old_rem == 0) {
        l_scb->is_full = 0;
        __a_clear_bit(&mcb->bitmap[mcb_slot / 64], (1ULL << (mcb_slot % 64)));
        if (__a_fetch_addu32(&mcb->rem_scb_count, 1) == 0) mcb->is_full = 0;
    }

    if (old_rem == MCB_SCB_COUNT - 1 && __atomic_load_n(&l_scb->rem_count, __ATOMIC_ACQUIRE) == MCB_SCB_COUNT) {
        if (__a_cas_p((volatile void*)&mcb->list_base[mcb_slot], l_scb, NULL) == l_scb) {
            push_deferred_scb(l_scb, 1);
        }
    }
}

static void _skyline_free_internal(void* ptr) {
    AllocBlock_t* header = (AllocBlock_t*)((uint64_t)ptr - sizeof(AllocBlock_t));

    ALLOCTOR_SECURITY_ASSERT(header->AllocPtrBaseAddress == (uint64_t)ptr);
    ALLOCTOR_SECURITY_ASSERT(header->BitMapBase > 0 && header->BitMapBase < SKYLINE_MAX_LEGAL_ADDR);
    ALLOCTOR_SECURITY_ASSERT(header->MCBAddr > 0 && header->MCBAddr < SKYLINE_MAX_LEGAL_ADDR);
    ALLOCTOR_SECURITY_ASSERT(header->Magic == ALLOC_BLOCK_MAGIC);

    uint64_t size_class  = header->AllocSizeAligned;
    uint32_t obj_bit_loc = header->BitMapBitLocation.SCBBitLocation;
    uint32_t mcb_slot    = header->BitMapBitLocation.MCBBitLocation;
    uint64_t scb_addr    = header->BitMapBase;
    uint64_t mcb_addr    = header->MCBAddr;
    uint64_t block_addr  = header->BlockAddr;

    int32_t idx = GetSizeClassIndex(size_class);
    if (idx < 0 || idx >= SCT_MAX_CLASSES) return;

    uint32_t word_idx = obj_bit_loc / 64;
    uint32_t bit_idx  = obj_bit_loc % 64;
    MainControlBlock_t* mcb = (MainControlBlock_t*)mcb_addr;

    if (SizeClassTable[idx][2] != 0) {
        // ==================== 小对象释放 ====================
        SecondControlBlock_t* scb = (SecondControlBlock_t*)scb_addr;
        ALLOCTOR_SECURITY_ASSERT(obj_bit_loc <= scb->bit_tail);

        if (!(__a_clear_bit(&scb->bitmap[word_idx], (1ULL << bit_idx)) & (1ULL << bit_idx))) return;

        uint64_t old_rem = __a_fetch_addu32(&scb->rem_count, 1);
        if (old_rem == 0) {
            scb->is_full = 0;
            __a_clear_bit(&mcb->bitmap[mcb_slot / 64], (1ULL << (mcb_slot % 64)));
            if (__a_fetch_addu32(&mcb->rem_scb_count, 1) == 0) mcb->is_full = 0;
        }

        uint64_t target_rem = scb->bit_tail + 1;
        if (old_rem == target_rem - 1 && __atomic_load_n(&scb->rem_count, __ATOMIC_ACQUIRE) == target_rem) {
            if (__a_cas_p((volatile void*)&mcb->list_base[mcb_slot], scb, NULL) == scb) {
                push_deferred_scb(scb, 0);
            }
        }
    } else {
        // ==================== 大对象释放 ====================
        if (tls_large_cache_cnt[idx] < LARGE_OBJ_CACHE_MAX) {
            *(void**)block_addr = tls_large_cache[idx];
            tls_large_cache[idx] = (void*)block_addr;
            tls_large_cache_cnt[idx]++;
            return;
        }
        _free_large_object_real((void*)block_addr, header);
    }
}

// ============================================================================
// 标准 API
// ============================================================================

void* malloc(size_t size) {
    int32_t slot = qsbr_enter();
    void* ptr = _skyline_malloc_internal(size);
    qsbr_leave(slot);
    return ptr;
}

void free(void* ptr) {
    if (!ptr) return;
    int32_t slot = qsbr_enter();
    _skyline_free_internal(ptr);
    qsbr_leave(slot);
}

void* calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > SIZE_MAX / nmemb) return NULL;
    size_t total_size = nmemb * size;
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

        if ((new_idx < old_idx && header->AllocSizeAligned - size > 4096) ||
            (SizeClassTable[old_idx][2] == 0 && new_idx != old_idx)) {
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