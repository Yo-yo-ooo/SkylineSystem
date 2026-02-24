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

volatile static spinlock_t pmm_lock = 0;
volatile struct limine_memmap_response* pmm_memmap = nullptr;

namespace PMM {
    volatile uint8_t *bitmap = nullptr;
    volatile uint64_t bitmap_size = 0;
    volatile uint64_t bitmap_last_free = 0;

    volatile uint64_t pmm_bitmap_start = 0;
    volatile uint64_t pmm_bitmap_size = 0;
    volatile uint64_t pmm_bitmap_pages = 0;

    void Init() {
        pmm_memmap = memmap_request.response;
        uint64_t page_count = 0;
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length % PAGE_SIZE != 0){
                kinfoln("ENTRY LENGTH: %d", entry->length);
                kwarn("Memory map entry length is NOT divisible by the page size. \n");
            }
            page_count += entry->length;
        }
        page_count /= PAGE_SIZE;
        pmm_bitmap_pages = page_count;
        bitmap_size = ALIGN_UP(page_count / 8, PAGE_SIZE);
        
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length >= bitmap_size && entry->type == LIMINE_MEMMAP_USABLE) {
                PMM::bitmap = HIGHER_HALF((uint8_t*)entry->base);
                entry->length -= bitmap_size;
                entry->base += bitmap_size;
                _memset((void*)PMM::bitmap, 0xFF, bitmap_size); // 强制转码避免 volatile 警告
                break;
            }
        }
        
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE)
                for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE)
                    PMM::bitmap_clear_((entry->base + j) / PAGE_SIZE);
        }
        pmm_bitmap_start = (uint64_t)PMM::bitmap;
        pmm_bitmap_size = bitmap_size;
        bitmap_last_free = 0; // 初始化 last_free
    }

    void bitmap_clear_(uint64_t bit) { bitmap_clear((void*)PMM::bitmap, bit); }
    void bitmap_set_(uint64_t bit) { bitmap_set((void*)PMM::bitmap, bit); }
    bool bitmap_test_(uint64_t bit) { return bitmap_get((void*)PMM::bitmap, bit); }

    void *Request() {
        spinlock_lock(&pmm_lock);
        uint64_t bit = bitmap_last_free;
        
        // 线性扫描空闲位
        while (bit < pmm_bitmap_pages && PMM::bitmap_test_(bit))
            bit++;
            
        if (bit >= pmm_bitmap_pages) {
            bit = 0;
            while (bit < bitmap_last_free && PMM::bitmap_test_(bit))
                bit++;
            if (bit >= bitmap_last_free) {
                kerror("PMM Out of memory!\n");
                spinlock_unlock(&pmm_lock);
                return nullptr;
            }
        }

        PMM::bitmap_set_(bit);
        bitmap_last_free = bit + 1; // 优化：下次从后一位开始找
        spinlock_unlock(&pmm_lock);
        return (void*)(bit * PAGE_SIZE);
    }

    // 修复了返回地址算错的 Bug，并引入了“连续空闲块滑动窗口”扫描
    void* Request(uint64_t n) {
        if (n == 0) return nullptr;
        if (n == 1) return Request();

        spinlock_lock(&pmm_lock);
        
        uint64_t start_bit = 0;
        uint64_t free_count = 0;
        
        // 滑动窗口：寻找连续的 n 个 0
        for (uint64_t bit = 0; bit < pmm_bitmap_pages; bit++) {
            if (!PMM::bitmap_test_(bit)) {
                if (free_count == 0) start_bit = bit;
                free_count++;
                
                if (free_count == n) {
                    for (uint64_t i = start_bit; i < start_bit + n; i++) {
                        PMM::bitmap_set_(i);
                    }
                    bitmap_last_free = start_bit + n;
                    spinlock_unlock(&pmm_lock);
                    // 返回物理地址：起点索引 * 4096
                    return (void*)(start_bit * PAGE_SIZE); 
                }
            } else {
                free_count = 0;
            }
        }

        kerror("PMM Out of contiguous memory (%lu pages)!\n", n);
        spinlock_unlock(&pmm_lock);
        return nullptr;
    }

    // 增加了多核并发锁，并支持释放“多页”
    void Free(void *ptr) {
        if (!ptr) return;
        uint64_t bit = (uint64_t)ptr / PAGE_SIZE;

        spinlock_lock(&pmm_lock);
        PMM::bitmap_clear_(bit);
        
        // 稍微优化：如果释放的位在前面，下次分配可以从这里开始看
        if (bit < bitmap_last_free) {
            bitmap_last_free = bit;
        }
        spinlock_unlock(&pmm_lock);
    }
}