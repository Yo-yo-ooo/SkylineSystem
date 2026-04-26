/*
* SPDX-License-Identifier: GPL-2.0-only
* File: pmm.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
* Optimized with: L1 Summary Bitmap + Hardware Acceleration + Architecture Split
*/
#include <mem/pmm.h>
#include <limine.h>
#include <klib/klib.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

volatile spinlock_t pmm_lock = 0;
volatile struct limine_memmap_response* pmm_memmap = nullptr;

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>

namespace PMM {
    volatile uint8_t *bitmap = nullptr;
    volatile uint64_t *bitmap_l1 = nullptr;

    volatile uint64_t bitmap_size = 0;
    volatile uint64_t bitmap_l1_size = 0;
    volatile uint64_t bitmap_last_free = 1;

    volatile uint64_t pmm_bitmap_start = 0;
    volatile uint64_t pmm_bitmap_size = 0;
    volatile uint64_t pmm_bitmap_pages = 0;

    static inline void bitmap_set_sync(uint64_t bit) {
        bitmap_set((void*)PMM::bitmap, bit);
        uint64_t idx64 = bit / 64;
        if (((uint64_t*)PMM::bitmap)[idx64] == 0xFFFFFFFFFFFFFFFF) {
            bitmap_set((void*)PMM::bitmap_l1, idx64);
        }
    }

    static inline void bitmap_clear_sync(uint64_t bit) {
        bitmap_clear((void*)PMM::bitmap, bit);
        bitmap_clear((void*)PMM::bitmap_l1, bit / 64);
    }

    void Init() {
        pmm_memmap = memmap_request.response;
        uint64_t page_count = 0;
        
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length % PAGE_SIZE != 0) {
                entry->length = (entry->length / PAGE_SIZE) * PAGE_SIZE;
            }
            page_count += entry->length;
        }
        
        pmm_bitmap_pages = page_count / PAGE_SIZE;
        bitmap_size = ALIGN_UP(pmm_bitmap_pages / 8, PAGE_SIZE);
        bitmap_l1_size = ALIGN_UP((pmm_bitmap_pages / 64) / 8, PAGE_SIZE);
        uint64_t total_meta_size = bitmap_size + bitmap_l1_size;
        
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length >= total_meta_size && entry->type == LIMINE_MEMMAP_USABLE) {
                PMM::bitmap = HIGHER_HALF((uint8_t*)entry->base);
                PMM::bitmap_l1 = (uint64_t*)((uint64_t)PMM::bitmap + bitmap_size);
                
                entry->length -= total_meta_size;
                entry->base += total_meta_size;
                
                _memset((void*)PMM::bitmap, 0xFF, bitmap_size); 
                _memset((void*)PMM::bitmap_l1, 0xFF, bitmap_l1_size);
                break;
            }
        }
        
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE) {
                for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE) {
                    bitmap_clear_sync((entry->base + j) / PAGE_SIZE);
                }
            }
        }
        
        bitmap_set_sync(0);
        pmm_bitmap_start = (uint64_t)PMM::bitmap;
        pmm_bitmap_size = bitmap_size;
        bitmap_last_free = 1; 
    }

    void* GlobalRequestSingle() {
        uint64_t start_idxL1 = (bitmap_last_free / 64) / 64;
        uint64_t max_idxL1 = bitmap_l1_size / 8;
        
        for (uint64_t i = start_idxL1; i < max_idxL1; i++) {
            if (bitmap_l1[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t idx64 = i * 64 + __builtin_ctzll(~bitmap_l1[i]);
                uint64_t absolute_bit = idx64 * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap)[idx64]);
                
                if (absolute_bit >= pmm_bitmap_pages) return nullptr;
                bitmap_set_sync(absolute_bit);
                bitmap_last_free = absolute_bit + 1;
                return (void*)(absolute_bit * PAGE_SIZE);
            }
        }
        
        for (uint64_t i = 0; i < start_idxL1; i++) {
            if (bitmap_l1[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t idx64 = i * 64 + __builtin_ctzll(~bitmap_l1[i]);
                uint64_t absolute_bit = idx64 * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap)[idx64]);
                
                if (absolute_bit >= pmm_bitmap_pages) return nullptr;
                bitmap_set_sync(absolute_bit);
                bitmap_last_free = absolute_bit + 1;
                return (void*)(absolute_bit * PAGE_SIZE);
            }
        }
        return nullptr;
    }

    void* Request(uint64_t n = 1) {
        if (n == 0) return nullptr;

        if (n == 1) {
            Interrupt::Mask();
            cpu_t* cpu = this_cpu(); 
            if (cpu && cpu->pmm_cache_count > 0) {
                return cpu->pmm_cache[--cpu->pmm_cache_count];
            }
            
            spinlock_lock(&pmm_lock);
            void* ret_page = GlobalRequestSingle(); 
            
            if (ret_page && cpu && cpu->pmm_cache_count < PMM_PCP_MAX) {
                for (int i = 0; i < PMM_PCP_BATCH; i++) {
                    if (cpu->pmm_cache_count >= PMM_PCP_MAX) break;
                    void* cache_page = GlobalRequestSingle();
                    if (!cache_page) break;
                    cpu->pmm_cache[cpu->pmm_cache_count++] = cache_page;
                }
            }
            spinlock_unlock(&pmm_lock);
            Interrupt::Unmask();
            return ret_page;
        }

        spinlock_lock(&pmm_lock);
        uint64_t start_bit = 0, free_count = 0;
        uint64_t current_bit = bitmap_last_free;
        bool wrapped = false;

        while (true) {
            if (current_bit >= pmm_bitmap_pages) {
                if (wrapped) break;
                current_bit = 0; wrapped = true; free_count = 0; continue;
            }

            uint64_t idxL1 = (current_bit / 64) / 64; 
            if (free_count == 0 && (current_bit % 4096 == 0) && (idxL1 < (pmm_bitmap_pages/4096))) {
                if (bitmap_l1[idxL1] == 0xFFFFFFFFFFFFFFFF) { current_bit += 4096; continue; }
            }

            uint64_t idx64 = current_bit / 64;
            if (free_count == 0 && (current_bit % 64 == 0)) {
                if (((uint64_t*)PMM::bitmap)[idx64] == 0xFFFFFFFFFFFFFFFF) { current_bit += 64; continue; }
            }

            if (!bitmap_get((void*)PMM::bitmap, current_bit)) {
                if (free_count == 0) start_bit = current_bit;
                if (++free_count == n) {
                    for (uint64_t i = start_bit; i < start_bit + n; i++) bitmap_set_sync(i);
                    bitmap_last_free = start_bit + n;
                    spinlock_unlock(&pmm_lock);
                    return (void*)(start_bit * PAGE_SIZE); 
                }
            } else {
                free_count = 0;
            }
            current_bit++;
        }
        kerror("PMM Out of contiguous memory (%lu pages)!\n", n);
        spinlock_unlock(&pmm_lock);
        return nullptr;
    }

    void Free(void *ptr, uint64_t n = 1) {
        if (!ptr || n == 0) return;

        if (n == 1) {
            // [TODO] Add interrupt_disable() here
            cpu_t* cpu = this_cpu();
            if (cpu) {
                if (cpu->pmm_cache_count < PMM_PCP_MAX) {
                    cpu->pmm_cache[cpu->pmm_cache_count++] = ptr;
                    return;
                }
                
                spinlock_lock(&pmm_lock);
                for (int i = 0; i < PMM_PCP_BATCH; i++) {
                    if (cpu->pmm_cache_count == 0) break;
                    uint64_t bit = (uint64_t)cpu->pmm_cache[--cpu->pmm_cache_count] / PAGE_SIZE;
                    bitmap_clear_sync(bit);
                    if (bit < bitmap_last_free) bitmap_last_free = bit; 
                }
                spinlock_unlock(&pmm_lock);
                
                cpu->pmm_cache[cpu->pmm_cache_count++] = ptr;
                return;
            }
        }

        uint64_t start_bit = (uint64_t)ptr / PAGE_SIZE;
        spinlock_lock(&pmm_lock);
        for (uint64_t i = 0; i < n; i++) bitmap_clear_sync(start_bit + i);
        if (start_bit < bitmap_last_free) bitmap_last_free = start_bit;
        spinlock_unlock(&pmm_lock);
    }
}

#else
// =======================================================================
// Fallback Branch: General Architecture (No SMP/PCP, Pure L1 Bitmap)
// =======================================================================

namespace PMM {
    volatile uint8_t *bitmap = nullptr;
    volatile uint64_t *bitmap_l1 = nullptr;

    volatile uint64_t bitmap_size = 0;
    volatile uint64_t bitmap_l1_size = 0;
    volatile uint64_t bitmap_last_free = 1;

    volatile uint64_t pmm_bitmap_start = 0;
    volatile uint64_t pmm_bitmap_size = 0;
    volatile uint64_t pmm_bitmap_pages = 0;

    static inline void bitmap_set_sync(uint64_t bit) {
        bitmap_set((void*)PMM::bitmap, bit);
        uint64_t idx64 = bit / 64;
        if (((uint64_t*)PMM::bitmap)[idx64] == 0xFFFFFFFFFFFFFFFF) {
            bitmap_set((void*)PMM::bitmap_l1, idx64);
        }
    }

    static inline void bitmap_clear_sync(uint64_t bit) {
        bitmap_clear((void*)PMM::bitmap, bit);
        bitmap_clear((void*)PMM::bitmap_l1, bit / 64);
    }

    void Init() {
        pmm_memmap = memmap_request.response;
        uint64_t page_count = 0;
        
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length % PAGE_SIZE != 0) {
                kinfoln("ENTRY LENGTH: %d", entry->length);
                kwarn("Memory map entry length is NOT divisible by the page size. \n");
                entry->length = (entry->length / PAGE_SIZE) * PAGE_SIZE;
            }
            page_count += entry->length;
        }
        
        pmm_bitmap_pages = page_count / PAGE_SIZE;
        bitmap_size = ALIGN_UP(pmm_bitmap_pages / 8, PAGE_SIZE);
        bitmap_l1_size = ALIGN_UP((pmm_bitmap_pages / 64) / 8, PAGE_SIZE);
        uint64_t total_meta_size = bitmap_size + bitmap_l1_size;
        
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length >= total_meta_size && entry->type == LIMINE_MEMMAP_USABLE) {
                PMM::bitmap = HIGHER_HALF((uint8_t*)entry->base);
                PMM::bitmap_l1 = (uint64_t*)((uint64_t)PMM::bitmap + bitmap_size);
                
                entry->length -= total_meta_size;
                entry->base += total_meta_size;
                
                _memset((void*)PMM::bitmap, 0xFF, bitmap_size); 
                _memset((void*)PMM::bitmap_l1, 0xFF, bitmap_l1_size);
                break;
            }
        }
        
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE) {
                for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE) {
                    bitmap_clear_sync((entry->base + j) / PAGE_SIZE);
                }
            }
        }
        
        bitmap_set_sync(0);
        pmm_bitmap_start = (uint64_t)PMM::bitmap;
        pmm_bitmap_size = bitmap_size;
        bitmap_last_free = 1; 
    }

    static void* GlobalRequestSingle() {
        uint64_t start_idxL1 = (bitmap_last_free / 64) / 64;
        uint64_t max_idxL1 = bitmap_l1_size / 8;
        
        for (uint64_t i = start_idxL1; i < max_idxL1; i++) {
            if (bitmap_l1[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t idx64 = i * 64 + __builtin_ctzll(~bitmap_l1[i]);
                uint64_t absolute_bit = idx64 * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap)[idx64]);
                
                if (absolute_bit >= pmm_bitmap_pages) return nullptr;
                bitmap_set_sync(absolute_bit);
                bitmap_last_free = absolute_bit + 1;
                return (void*)(absolute_bit * PAGE_SIZE);
            }
        }
        
        for (uint64_t i = 0; i < start_idxL1; i++) {
            if (bitmap_l1[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t idx64 = i * 64 + __builtin_ctzll(~bitmap_l1[i]);
                uint64_t absolute_bit = idx64 * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap)[idx64]);
                
                if (absolute_bit >= pmm_bitmap_pages) return nullptr;
                bitmap_set_sync(absolute_bit);
                bitmap_last_free = absolute_bit + 1;
                return (void*)(absolute_bit * PAGE_SIZE);
            }
        }
        return nullptr;
    }

    void* Request(uint64_t n = 1) {
        if (n == 0) return nullptr;

        spinlock_lock(&pmm_lock);
        
        if (n == 1) {
            void* ret = GlobalRequestSingle();
            spinlock_unlock(&pmm_lock);
            return ret;
        }

        uint64_t start_bit = 0, free_count = 0;
        uint64_t current_bit = bitmap_last_free;
        bool wrapped = false;

        while (true) {
            if (current_bit >= pmm_bitmap_pages) {
                if (wrapped) break;
                current_bit = 0; wrapped = true; free_count = 0; continue;
            }

            uint64_t idxL1 = (current_bit / 64) / 64; 
            if (free_count == 0 && (current_bit % 4096 == 0) && (idxL1 < (pmm_bitmap_pages/4096))) {
                if (bitmap_l1[idxL1] == 0xFFFFFFFFFFFFFFFF) { current_bit += 4096; continue; }
            }

            uint64_t idx64 = current_bit / 64;
            if (free_count == 0 && (current_bit % 64 == 0)) {
                if (((uint64_t*)PMM::bitmap)[idx64] == 0xFFFFFFFFFFFFFFFF) { current_bit += 64; continue; }
            }

            if (!bitmap_get((void*)PMM::bitmap, current_bit)) {
                if (free_count == 0) start_bit = current_bit;
                if (++free_count == n) {
                    for (uint64_t i = start_bit; i < start_bit + n; i++) bitmap_set_sync(i);
                    bitmap_last_free = start_bit + n;
                    spinlock_unlock(&pmm_lock);
                    return (void*)(start_bit * PAGE_SIZE); 
                }
            } else {
                free_count = 0;
            }
            current_bit++;
        }
        kerror("PMM Out of contiguous memory (%lu pages)!\n", n);
        spinlock_unlock(&pmm_lock);
        return nullptr;
    }

    void Free(void *ptr, uint64_t n = 1) {
        if (!ptr || n == 0) return;
        uint64_t start_bit = (uint64_t)ptr / PAGE_SIZE;
        
        spinlock_lock(&pmm_lock);
        for (uint64_t i = 0; i < n; i++) bitmap_clear_sync(start_bit + i);
        if (start_bit < bitmap_last_free) bitmap_last_free = start_bit;
        spinlock_unlock(&pmm_lock);
    }
}
#endif