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

#pragma GCC push_options
#pragma GCC optimize ("O0")

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

volatile spinlock_t pmm_lock = 0;
volatile struct limine_memmap_response* pmm_memmap = nullptr;

namespace PMM{
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
                kinfoln("ENTRY LENGTH: %d",entry->length);
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
                _memset(PMM::bitmap, 0xFF, bitmap_size);
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
    }

    void bitmap_clear_(uint64_t bit) {
        bitmap_clear(PMM::bitmap,bit);
    }

    void bitmap_set_(uint64_t bit) {
        bitmap_set(PMM::bitmap,bit);
    }

    bool bitmap_test_(uint64_t bit) {
        return bitmap_get(PMM::bitmap,bit);
    }

    void *Request() {
        spinlock_lock(&pmm_lock);
        // TODO: Handle when we can't find a free bit after bitmap_last_free
        // (reiterate through the bitmap, and if we dont find ANY frees, we panic.)
        uint64_t bit = bitmap_last_free;
        while (bit < pmm_bitmap_pages && PMM::bitmap_test_(bit))
            bit++;
        if (bit >= pmm_bitmap_pages) {
            bit = 0;
            while (bit < bitmap_last_free && PMM::bitmap_test_(bit))
                bit++;
            if (bit == bitmap_last_free) {
                kerror("PMM Out of memory!\n");
                spinlock_unlock(&pmm_lock);
                return nullptr;
            }
        }
        PMM::bitmap_set_(bit);
        bitmap_last_free = bit;
        spinlock_unlock(&pmm_lock);
        return (void*)(bit * PAGE_SIZE);
    }

    void* Request(uint64_t n){
        uint64_t bit;
        for(uint64_t j = 0;j < n;++j){
            bit = bitmap_last_free;
            while (bit < pmm_bitmap_pages && PMM::bitmap_test_(bit))
                bit++;
            if (bit >= pmm_bitmap_pages) {
                bit = 0;
                while (bit < bitmap_last_free && PMM::bitmap_test_(bit))
                    bit++;
                if (bit == bitmap_last_free) {
                    kerror("PMM Out of memory!\n");
                    return nullptr;
                }
            }
            PMM::bitmap_set_(bit);
            bitmap_last_free = bit;
        }
        return (void*)(bit * n * PAGE_SIZE);
    }


    void Free(void *ptr) {
        uint64_t bit = (uint64_t)ptr / PAGE_SIZE;
        PMM::bitmap_clear_(bit);
    }


}

#pragma GCC pop_options