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

// ============================================================================
// QSBR (Quiescent State Based Reclamation) 无锁内存延迟回收子系统
// ============================================================================

#define QSBR_SLOTS 32
typedef struct {
    volatile uint64_t count;
    char padding[56]; // 强制缓存行对齐，彻底消除并发伪共享 (False Sharing)
} qsbr_slot_t;

static qsbr_slot_t qsbr_counters[QSBR_SLOTS] __attribute__((aligned(64)));
static volatile void* deferred_scb_list = NULL;
static volatile uint32_t gc_lock = 0;

// 高速无锁通行证，使用 rdtsc 硬件哈希极速打散竞争热点
static inline int qsbr_enter() {
    uint32_t hash = (uint32_t)(__builtin_ia32_rdtsc() % QSBR_SLOTS);
    __a_fetch_addu64(&qsbr_counters[hash].count, 1);
    return hash;
}

static inline void qsbr_leave(int hash) {
    __a_subu64(&qsbr_counters[hash].count, 1);
}

// 检查当前系统是否处于没有任何线程在分配器内部的“绝对安全静态点”
static inline int is_quiescent() {
    for (int i = 0; i < QSBR_SLOTS; i++) {
        if (__atomic_load_n(&qsbr_counters[i].count, __ATOMIC_ACQUIRE) != 0) return 0;
    }
    return 1;
}

// 侵入式链表暂存被废弃的 SCB（不额外申请任何内存）
static void push_deferred_gc(volatile void** list_head, void* block) {
    void* old_head;
    do {
        old_head = (void*)*list_head;
        *(void**)block = old_head; // 复用被废弃元数据块的前 8 字节作为 Next 指针
    } while (__a_cas_p(list_head, old_head, block) != old_head);
}

// 触发静态检查点垃圾回收，物理级释放内存
static inline void try_gc() {
    uint32_t expected = 0;
    if (__atomic_compare_exchange_n(&gc_lock, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        if (is_quiescent()) {
            void * scb_list = (void *)(uintptr_t)__atomic_exchange_n(&deferred_scb_list, NULL, __ATOMIC_ACQ_REL);
            while (scb_list) {
                void* next = *(void**)scb_list;
                LessCore(scb_list, 4); // 此时绝对没有任何线程会访问到这里，安全呼叫内核释放
                scb_list = next;
            }
        }
        __atomic_store_n(&gc_lock, 0, __ATOMIC_RELEASE);
    }
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
// 内部逻辑：实际的分配与释放引擎
// ============================================================================

static void* _skyline_malloc_internal(size_t size) {
    if (size == 0 || size > 9223372036854775808ULL) return NULL;

    int idx = GetSizeClassIndex(size);
    if (idx < 0 || idx >= 75) return NULL;
    
    uint64_t size_class   = SizeClassTable[idx][0];
    uint64_t region_size  = SizeClassTable[idx][2]; 
    void* allocated_ptr   = NULL;

    while (!allocated_ptr) {
        
        // --------------------------------------------------------------------
        // Tier 1: MCB 中央链表检索与无锁尾插
        // --------------------------------------------------------------------
        MainControlBlock_t* mcb = (MainControlBlock_t*)SizeClassTable[idx][1];
        MainControlBlock_t* prev_mcb = NULL; 

        while (mcb) {
            uint64_t is_full = mcb->is_full;
            if (is_full == 0) break; 
            
            // [架构优化]: 取消 MCB 的 unlinking 逻辑。MCB 作为极小内存消耗的分配器骨架永远常驻，
            // 彻底消灭了 MCB 级联解绑时的并发风暴和复杂 Race Condition。
            prev_mcb = mcb;
            mcb = (MainControlBlock_t*)mcb->next; 
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
                LessCore(new_mcb, 4); // CAS 失败，内存从未公开暴露，立即 LessCore 是 100% 安全的
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

        // ====================================================================
        // Tier 2: 架构分流与无锁元数据注入
        // ====================================================================
        int is_scb_full = 0;

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
                
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[scb_idx], NULL, scb);
                if (actual != NULL) {
                    LessCore(data_region, ((total_objects * step_size) + 4095) / 4096);
                    LessCore(scb, 4);
                    scb = (SecondControlBlock_t*)actual;
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
                        // [关键修复]: Double-Check 逻辑脱轨检测。防止我们抢到了位，但 SCB 刚好被其他线程 Unlink。
                        if (__atomic_load_n((uint64_t*)&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE) != (uint64_t)scb) {
                            uint64_t rb_old, rb_new;
                            do { // 状态脱离，回滚位图修改并安全重新大循环
                                rb_old = scb->bitmap[i];
                                rb_new = rb_old & ~(1ULL << free_bit);
                            } while (A_CAS_U64_ASM(&scb->bitmap[i], rb_old, rb_new) != rb_old);
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

            uint64_t old_rem = __a_subu64(&scb->rem_count, 1); 
            if (old_rem == 1) { 
                is_scb_full = 1;
                scb->is_full = 1; 
            }
        } 
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
                        // [Double Check 保驾护航]
                        if (__atomic_load_n((uint64_t*)&mcb->list_base[scb_idx], __ATOMIC_ACQUIRE) != (uint64_t)l_scb) {
                            uint64_t rb_old, rb_new;
                            do {
                                rb_old = l_scb->bitmap[i];
                                rb_new = rb_old & ~(1ULL << free_bit);
                            } while (A_CAS_U64_ASM(&l_scb->bitmap[i], rb_old, rb_new) != rb_old);
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
                
                uint64_t old_val = __atomic_load_n(&l_scb->bitmap[word_idx], __ATOMIC_ACQUIRE);
                if (!(old_val & mask)) return NULL;
                
                uint64_t new_val;
                while (1) {
                    new_val = old_val & ~mask;
                    uint64_t actual_old = A_CAS_U64_ASM(&l_scb->bitmap[word_idx], old_val, new_val);
                    if (actual_old == old_val) break;
                    old_val = actual_old;
                    if (!(old_val & mask)) break;
                    CPU_RELAX(); 
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
            
            l_scb->list_base[obj_idx] = (uint64_t)page_start;
        }

        // --------------------------------------------------------------------
        // Tier 3: 级联状态逆向反馈
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
        
        uint64_t old_bitmap = __a_and_64(&scb->bitmap[word_idx], ~(1ULL << bit_idx));
        if (!(old_bitmap & (1ULL << bit_idx))) return; 

        uint64_t old_rem = __a_fetch_addu64(&scb->rem_count, 1); 

        if (old_rem == 0) {
            scb->is_full = 0; 
            int mcb_word = mcb_slot / 64;
            int mcb_bit  = mcb_slot % 64;
            
            __a_and_64(&mcb->bitmap[mcb_word], ~(1ULL << mcb_bit));
            uint32_t old_mcb_rem = __a_fetch_addu32(&mcb->rem_scb_count, 1); 
            
            if (old_mcb_rem == 0) mcb->is_full = 0;
        }

        if(old_rem == 2013) {
            uint64_t current_rem = __atomic_load_n(&scb->rem_count, __ATOMIC_ACQUIRE);
            if (current_rem == 2014) {
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[mcb_slot], scb, NULL);
                if (actual == scb) {
                    // 解绑成功，压入 QSBR 延迟队列等待安全回收
                    push_deferred_gc(&deferred_scb_list, scb);
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
        LessCore((void*)block_addr, pages_needed); // 大对象数据页不存在并发遍历，直接还给 OS，安全。

        uint64_t old_bitmap = __a_and_64(&l_scb->bitmap[word_idx], ~(1ULL << bit_idx));
        if (!(old_bitmap & (1ULL << bit_idx))) return; 
        
        uint64_t old_rem = __a_fetch_addu64((volatile uint64_t*)&l_scb->rem_count, 1); 

        if (old_rem == 0) {
            l_scb->is_full = 0; 
            int mcb_word = mcb_slot / 64;
            int mcb_bit  = mcb_slot % 64;

            __a_and_64(&mcb->bitmap[mcb_word], ~(1ULL << mcb_bit));
            uint32_t old_mcb_rem = __a_fetch_addu32(&mcb->rem_scb_count, 1);
            
            if (old_mcb_rem == 0) mcb->is_full = 0;
        }

        if(old_rem == 2013) {
            uint64_t current_rem = __atomic_load_n(&l_scb->rem_count, __ATOMIC_ACQUIRE);
            if (current_rem == 2014) {
                void* actual = __a_cas_p((volatile void*)&mcb->list_base[mcb_slot], l_scb, NULL);
                if (actual == l_scb) {
                    // 解绑成功，压入 QSBR
                    push_deferred_gc(&deferred_scb_list, l_scb);
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
    
    // 脱离无锁结构访问期后，顺手尝试进行垃圾清理。
    // 这让物理内存能够在系统并发压力降维时立刻回流 OS。
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