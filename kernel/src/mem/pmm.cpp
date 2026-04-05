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

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h> // 引入 this_cpu() 和 cpu_t
#endif

namespace PMM {
    volatile uint8_t *bitmap = nullptr;
    volatile uint64_t bitmap_size = 0;
    volatile uint64_t bitmap_last_free = 0; // 核心游标

    volatile uint64_t pmm_bitmap_start = 0;
    volatile uint64_t pmm_bitmap_size = 0;
    volatile uint64_t pmm_bitmap_pages = 0;

    void Init() {
        pmm_memmap = memmap_request.response;
        uint64_t page_count = 0;
        
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            
            // 严格页对齐 (基址也要对齐，防止偏移错乱)
            if (entry->base % PAGE_SIZE != 0) {
                uint64_t diff = PAGE_SIZE - (entry->base % PAGE_SIZE);
                entry->base += diff;
                entry->length -= diff;
            }
            if (entry->length % PAGE_SIZE != 0) {
                entry->length = (entry->length / PAGE_SIZE) * PAGE_SIZE;
            }
            page_count += entry->length;
        }
        
        page_count /= PAGE_SIZE;
        pmm_bitmap_pages = page_count;
        bitmap_size = ALIGN_UP(page_count / 8, PAGE_SIZE);
        
        // 寻找足够大的连续可用内存来放置位图本身
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length >= bitmap_size && entry->type == LIMINE_MEMMAP_USABLE) {
                PMM::bitmap = HIGHER_HALF((uint8_t*)entry->base);
                entry->length -= bitmap_size;
                entry->base += bitmap_size;
                _memset((void*)PMM::bitmap, 0xFF, bitmap_size); // 默认全满
                break;
            }
        }
        
        // 标记可用区域为 0 (空闲)
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->type == LIMINE_MEMMAP_USABLE) {
                for (uint64_t j = 0; j < entry->length; j += PAGE_SIZE) {
                    PMM::bitmap_clear_((entry->base + j) / PAGE_SIZE);
                }
            }
        }
        
        // 保护第 0 页 (物理地址 0x0)，防止返回 nullptr 被误判为失败
        PMM::bitmap_set_(0); 

        pmm_bitmap_start = (uint64_t)PMM::bitmap;
        pmm_bitmap_size = bitmap_size;
        bitmap_last_free = 1; // 从第 1 页开始找
    }

    void bitmap_clear_(uint64_t bit) { bitmap_clear((void*)PMM::bitmap, bit); }
    void bitmap_set_(uint64_t bit) { bitmap_set((void*)PMM::bitmap, bit); }
    bool bitmap_test_(uint64_t bit) { return bitmap_get((void*)PMM::bitmap, bit); }

    // 内部函数：强制从全局位图申请单页 (带锁环境调用)
    static void* GlobalRequestSingle() {
        uint64_t* map64 = (uint64_t*)PMM::bitmap;
        uint64_t max_idx = pmm_bitmap_pages / 64;
        uint64_t start_idx = bitmap_last_free / 64;

        // 环形扫描：先扫后半段，再扫前半段
        for (uint64_t i = start_idx; i <= max_idx; i++) {
            if (map64[i] != 0xFFFFFFFFFFFFFFFF) { 
#ifdef __x86_64__
                uint64_t bit_offset = __builtin_ctzll(~map64[i]); 
#else
                // 通用兜底：如果没有 ctzll 指令，手动找 0
                uint64_t bit_offset = 0;
                while ((map64[i] & (1ULL << bit_offset))) bit_offset++;
#endif
                uint64_t absolute_bit = i * 64 + bit_offset;
                if (absolute_bit >= pmm_bitmap_pages) break;
                PMM::bitmap_set_(absolute_bit);
                bitmap_last_free = absolute_bit + 1;
                return (void*)(absolute_bit * PAGE_SIZE);
            }
        }

        for (uint64_t i = 0; i < start_idx; i++) {
            if (map64[i] != 0xFFFFFFFFFFFFFFFF) {
#ifdef __x86_64__
                uint64_t bit_offset = __builtin_ctzll(~map64[i]);
#else
                uint64_t bit_offset = 0;
                while ((map64[i] & (1ULL << bit_offset))) bit_offset++;
#endif
                uint64_t absolute_bit = i * 64 + bit_offset;
                if (absolute_bit >= pmm_bitmap_pages) break;
                PMM::bitmap_set_(absolute_bit);
                bitmap_last_free = absolute_bit + 1;
                return (void*)(absolute_bit * PAGE_SIZE);
            }
        }
        return nullptr;
    }

    void* Request(uint64_t n = 1) {
        if (n == 0) return nullptr;

        if (n == 1) {
#ifdef __x86_64__
            // [需根据你的 klib 修改]: 关中断，保存中断状态 (解决 0x180 Bug 的关键)
            // bool irq_state = interrupt_disable(); 
            cpu_t* cpu = this_cpu(); 
            
            if (cpu && cpu->pmm_cache_count > 0) {
                void* ret = cpu->pmm_cache[--cpu->pmm_cache_count];
                // interrupt_restore(irq_state); // 恢复中断
                return ret;
            }
            // interrupt_restore(irq_state);
#endif
            
            spinlock_lock(&pmm_lock);
            void* ret_page = GlobalRequestSingle(); 
            
#ifdef __x86_64__
            // 检查 cpu 不为 NULL，并且确保不会溢出 PCP 数组
            if (ret_page && cpu && cpu->pmm_cache_count < PMM_PCP_MAX) {
                for (int i = 0; i < PMM_PCP_BATCH; i++) {
                    if (cpu->pmm_cache_count >= PMM_PCP_MAX) break; // 终极越界防护
                    void* cache_page = GlobalRequestSingle();
                    if (!cache_page) break; 
                    cpu->pmm_cache[cpu->pmm_cache_count++] = cache_page;
                }
            }
#endif
            spinlock_unlock(&pmm_lock);
            return ret_page;
        }

        // --- 多页申请逻辑：双向环形滑动窗口 + 64位跳远 ---
        spinlock_lock(&pmm_lock);
        uint64_t start_bit = 0, free_count = 0;
        uint64_t* map64 = (uint64_t*)PMM::bitmap;
        
        uint64_t current_bit = bitmap_last_free;
        bool wrapped = false;

        while (true) {
            // 环形折返逻辑
            if (current_bit >= pmm_bitmap_pages) {
                if (wrapped) break; // 绕一圈了，真没内存了
                current_bit = 0;
                wrapped = true;
                free_count = 0;
                continue;
            }

            // 性能核心：跳过满载的 64 页 (不分架构，全都适用)
            if (free_count == 0 && (current_bit % 64 == 0) && (current_bit / 64) < (pmm_bitmap_pages / 64)) {
                if (map64[current_bit / 64] == 0xFFFFFFFFFFFFFFFF) {
                    current_bit += 64; 
                    continue; 
                }
            }

            if (!PMM::bitmap_test_(current_bit)) {
                if (free_count == 0) start_bit = current_bit;
                free_count++;
                
                if (free_count == n) {
                    for (uint64_t i = start_bit; i < start_bit + n; i++) PMM::bitmap_set_(i);
                    bitmap_last_free = start_bit + n;
                    spinlock_unlock(&pmm_lock);
                    return (void*)(start_bit * PAGE_SIZE); 
                }
            } else {
                free_count = 0; // 遇到占用，立刻清零
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
#ifdef __x86_64__
            // [需根据你的 klib 修改]: 关中断
            // bool irq_state = interrupt_disable();
            cpu_t* cpu = this_cpu();
            if (cpu) {
                if (cpu->pmm_cache_count < PMM_PCP_MAX) {
                    cpu->pmm_cache[cpu->pmm_cache_count++] = ptr;
                    // interrupt_restore(irq_state);
                    return;
                }
                
                // PCP 满了，需要退货 (退货前先恢复中断，避免长时间持有全局锁时关中断)
                // interrupt_restore(irq_state);
                
                spinlock_lock(&pmm_lock);
                for (int i = 0; i < PMM_PCP_BATCH; i++) {
                    // 退货时也加上防下溢检查
                    if (cpu->pmm_cache_count == 0) break;
                    uint64_t bit = (uint64_t)cpu->pmm_cache[--cpu->pmm_cache_count] / PAGE_SIZE;
                    PMM::bitmap_clear_(bit);
                    if (bit < bitmap_last_free) bitmap_last_free = bit; 
                }
                spinlock_unlock(&pmm_lock);
                
                // 退完货有了空间，重新关中断放入当前页
                // irq_state = interrupt_disable();
                cpu->pmm_cache[cpu->pmm_cache_count++] = ptr;
                // interrupt_restore(irq_state);
                return;
            }
            // interrupt_restore(irq_state);
#endif
        }

        // --- 多页释放或无 PCP 时回退到全局位图 ---
        uint64_t start_bit = (uint64_t)ptr / PAGE_SIZE;
        spinlock_lock(&pmm_lock);
        for (uint64_t i = 0; i < n; i++) {
            PMM::bitmap_clear_(start_bit + i);
        }
        // 如果释放的内存比当前的游标低，把游标拉回来，防止前面出现内存空洞
        if (start_bit < bitmap_last_free) bitmap_last_free = start_bit;
        spinlock_unlock(&pmm_lock);
    }
}