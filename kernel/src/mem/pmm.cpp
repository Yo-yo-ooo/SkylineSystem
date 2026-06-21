/*
* SPDX-License-Identifier: GPL-2.0-only
* File: pmm.cpp
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

volatile uint64_t huge_last_free = 0;  // 大页分配缓存指针

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

    // --- 批量提速核心 ---
    static void bitmap_clear_range(uint64_t start_bit, uint64_t count) {
        uint64_t current = start_bit;
        while (current < start_bit + count) {
            if (current % 64 == 0 && (start_bit + count - current) >= 64) {
                ((uint64_t*)PMM::bitmap)[current / 64] = 0;
                bitmap_clear((void*)PMM::bitmap_l1, current / 64);
                current += 64;
            } else {
                bitmap_clear_sync(current);
                current++;
            }
        }
    }

    static void bitmap_set_range(uint64_t start_bit, uint64_t count) {
        uint64_t current = start_bit;
        while (current < start_bit + count) {
            if (current % 64 == 0 && (start_bit + count - current) >= 64) {
                ((uint64_t*)PMM::bitmap)[current / 64] = 0xFFFFFFFFFFFFFFFF;
                bitmap_set((void*)PMM::bitmap_l1, current / 64);
                current += 64;
            } else {
                bitmap_set_sync(current);
                current++;
            }
        }
    }

    void Init() {
        pmm_memmap = memmap_request.response;
        uint64_t max_phys_addr = 0;

        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            uint64_t top = entry->base + entry->length;
            if (top > max_phys_addr) {
                max_phys_addr = top;
            }
        }

        pmm_bitmap_pages = max_phys_addr / PAGE_SIZE;
        bitmap_size = ALIGN_UP(pmm_bitmap_pages / 8, PAGE_SIZE);
        bitmap_l1_size = ALIGN_UP((pmm_bitmap_pages / 64) / 8, PAGE_SIZE);
        uint64_t total_meta_size = bitmap_size + bitmap_l1_size;

        bool meta_placed = false;
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= total_meta_size) {
                PMM::bitmap = (uint8_t*)HIGHER_HALF(entry->base);
                PMM::bitmap_l1 = (uint64_t*)((uintptr_t)PMM::bitmap + bitmap_size);

                memset_fscpuf((void*)PMM::bitmap, 0xFF, bitmap_size);
                memset_fscpuf((void*)PMM::bitmap_l1, 0xFF, bitmap_l1_size);

                entry->base += total_meta_size;
                entry->length -= total_meta_size;
                
                meta_placed = true;
                break;
            }
        }

        if (!meta_placed) {
            Panic("PMM: Failed to allocate metadata bitmap!");
        }

        // 使用 bitmap_clear_range 极大加速状态同步
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE) {
                uint64_t start_page = entry->base / PAGE_SIZE;
                uint64_t page_count = entry->length / PAGE_SIZE;
                
                if (start_page < pmm_bitmap_pages) {
                    uint64_t count = (start_page + page_count > pmm_bitmap_pages) 
                                   ? (pmm_bitmap_pages - start_page) : page_count;
                    bitmap_clear_range(start_page, count);
                }
            }
        }

        bitmap_set_sync(0); 
        pmm_bitmap_start = (uintptr_t)PMM::bitmap;
        pmm_bitmap_size = bitmap_size;
        bitmap_last_free = 1; 
    }

        void* RequestHuge() {
        spinlock_lock(&pmm_lock);
        
        uint64_t max_align_idx = pmm_bitmap_pages / 512;
        uint64_t start_idx = huge_last_free / 512;
        bool wrapped = false;

        for (uint64_t i = start_idx; ; i++) {
            if (i >= max_align_idx) {
                if (wrapped) break;
                i = 0;
                wrapped = true;
            }
            
            uint64_t base_idxL0 = i * 8; 
            bool is_free = true;
            for (int j = 0; j < 8; j++) {
                if (((uint64_t*)PMM::bitmap)[base_idxL0 + j] != 0) {
                    is_free = false;
                    break;
                }
            }
            
            if (is_free) {
                for (int j = 0; j < 8; j++) {
                    ((uint64_t*)PMM::bitmap)[base_idxL0 + j] = 0xFFFFFFFFFFFFFFFF;
                    bitmap_set((void*)PMM::bitmap_l1, base_idxL0 + j); 
                }
                
                uint64_t allocated_page_idx = i * 512;
                huge_last_free = allocated_page_idx + 512;
                bitmap_last_free = allocated_page_idx + 512;
                spinlock_unlock(&pmm_lock);
                return (void*)(allocated_page_idx * PAGE_SIZE);
            }
        }
        
        spinlock_unlock(&pmm_lock);
        return nullptr; 
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
                    bitmap_set_range(start_bit, n);
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
        bitmap_clear_range(start_bit, n);
        if (start_bit < bitmap_last_free) bitmap_last_free = start_bit;
        spinlock_unlock(&pmm_lock);
    }
    
    void FreeHuge(void *ptr) {
        Free(ptr, 512); 
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

    static void bitmap_clear_range(uint64_t start_bit, uint64_t count) {
        uint64_t current = start_bit;
        while (current < start_bit + count) {
            if (current % 64 == 0 && (start_bit + count - current) >= 64) {
                ((uint64_t*)PMM::bitmap)[current / 64] = 0;
                bitmap_clear((void*)PMM::bitmap_l1, current / 64);
                current += 64;
            } else {
                bitmap_clear_sync(current);
                current++;
            }
        }
    }

    static void bitmap_set_range(uint64_t start_bit, uint64_t count) {
        uint64_t current = start_bit;
        while (current < start_bit + count) {
            if (current % 64 == 0 && (start_bit + count - current) >= 64) {
                ((uint64_t*)PMM::bitmap)[current / 64] = 0xFFFFFFFFFFFFFFFF;
                bitmap_set((void*)PMM::bitmap_l1, current / 64);
                current += 64;
            } else {
                bitmap_set_sync(current);
                current++;
            }
        }
    }

    void Init() {
        pmm_memmap = memmap_request.response;
        uint64_t max_phys_addr = 0;
        
        // 此处修正为使用 max_phys_addr，兼容具有内存空洞(Holes)的通用架构设备
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length % PAGE_SIZE != 0) {
                kinfoln("ENTRY LENGTH: %d", entry->length);
                kwarn("Memory map entry length is NOT divisible by the page size. \n");
                entry->length = (entry->length / PAGE_SIZE) * PAGE_SIZE;
            }
            uint64_t top = entry->base + entry->length;
            if (top > max_phys_addr) {
                max_phys_addr = top;
            }
        }
        
        pmm_bitmap_pages = max_phys_addr / PAGE_SIZE;
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
                uint64_t start_page = entry->base / PAGE_SIZE;
                uint64_t page_count = entry->length / PAGE_SIZE;
                
                if (start_page < pmm_bitmap_pages) {
                    uint64_t count = (start_page + page_count > pmm_bitmap_pages) 
                                   ? (pmm_bitmap_pages - start_page) : page_count;
                    bitmap_clear_range(start_page, count);
                }
            }
        }
        
        bitmap_set_sync(0);
        pmm_bitmap_start = (uint64_t)PMM::bitmap;
        pmm_bitmap_size = bitmap_size;
        bitmap_last_free = 1; 
    }

        void* RequestHuge() {
        spinlock_lock(&pmm_lock);
        
        uint64_t max_align_idx = pmm_bitmap_pages / 512;
        uint64_t start_idx = huge_last_free / 512;
        bool wrapped = false;

        for (uint64_t i = start_idx; ; i++) {
            if (i >= max_align_idx) {
                if (wrapped) break;
                i = 0;
                wrapped = true;
            }
            
            uint64_t base_idxL0 = i * 8; 
            bool is_free = true;
            for (int j = 0; j < 8; j++) {
                if (((uint64_t*)PMM::bitmap)[base_idxL0 + j] != 0) {
                    is_free = false;
                    break;
                }
            }
            
            if (is_free) {
                for (int j = 0; j < 8; j++) {
                    ((uint64_t*)PMM::bitmap)[base_idxL0 + j] = 0xFFFFFFFFFFFFFFFF;
                    bitmap_set((void*)PMM::bitmap_l1, base_idxL0 + j); 
                }
                
                uint64_t allocated_page_idx = i * 512;
                huge_last_free = allocated_page_idx + 512;
                bitmap_last_free = allocated_page_idx + 512;
                spinlock_unlock(&pmm_lock);
                return (void*)(allocated_page_idx * PAGE_SIZE);
            }
        }
        
        spinlock_unlock(&pmm_lock);
        return nullptr; 
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
                    bitmap_set_range(start_bit, n);
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
        bitmap_clear_range(start_bit, n);
        if (start_bit < bitmap_last_free) bitmap_last_free = start_bit;
        spinlock_unlock(&pmm_lock);
    }

    void FreeHuge(void *ptr) {
        Free(ptr, 512);
    }
}
#endif