//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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

// --- 3-Level Bitmap Configuration ---
// L1: 1 bit = 1 page (4KB)
// L2: 1 bit = 512 L1 bits = 2MB
// L3: 1 bit = 1024 L2 bits = 2GB
#define L1_BITS_PER_L2_BIT 512
#define L2_BITS_PER_L3_BIT 1024

#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>

namespace PMM {
    volatile uint8_t *bitmap = nullptr;
    volatile uint64_t *bitmap_l2 = nullptr;
    volatile uint64_t *bitmap_l3 = nullptr;
    // --- Delayed init tracker: 1 bit per L2 (2MB) block. 0 = L1 region not yet populated. ---
    volatile uint8_t *init_bitmap = nullptr;
    volatile uint64_t init_bitmap_size = 0;

    volatile uint64_t bitmap_size = 0;
    volatile uint64_t bitmap_l2_size = 0;
    volatile uint64_t bitmap_l3_size = 0;
    volatile uint64_t bitmap_last_free = 1;

    volatile uint64_t pmm_bitmap_start = 0;
    volatile uint64_t pmm_bitmap_size = 0;
    volatile uint64_t pmm_bitmap_pages = 0;

    // ===================================================================
    // Delayed L1 initialization + lazy occupancy marking
    // Populates 512 L1 bits of a single 2MB block on first access by
    // consulting the memmap. Idempotent and race-free under pmm_lock.
    // ===================================================================
    static inline void ensure_l2_init(uint64_t l2_bit) {
        if (bitmap_get((void*)PMM::init_bitmap, l2_bit)) return;

        uint64_t l1_start = l2_bit * L1_BITS_PER_L2_BIT;
        uint64_t l1_end   = l1_start + L1_BITS_PER_L2_BIT;
        if (l1_end > pmm_bitmap_pages) l1_end = pmm_bitmap_pages;

        if (l1_start < pmm_bitmap_pages) {
            // Default: every page in this 2MB block considered occupied
            for (uint64_t i = l1_start; i < l1_end; i++) {
                bitmap_set((void*)PMM::bitmap, i);
            }

            // Lazy occupancy marking: clear L1 bits for pages that the
            // firmware reports as USABLE within this 2MB region.
            uint64_t region_start_addr = l1_start * PAGE_SIZE;
            uint64_t region_end_addr   = l1_end   * PAGE_SIZE;

            for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
                struct limine_memmap_entry *entry = pmm_memmap->entries[i];
                if (entry->type != LIMINE_MEMMAP_USABLE) continue;

                uint64_t entry_start = entry->base;
                uint64_t entry_end   = entry->base + entry->length;
                if (entry_end <= region_start_addr || entry_start >= region_end_addr) continue;

                uint64_t ov_start = entry_start > region_start_addr ? entry_start : region_start_addr;
                uint64_t ov_end   = entry_end   < region_end_addr   ? entry_end   : region_end_addr;

                // Page-align the overlap window
                ov_start = (ov_start + (PAGE_SIZE - 1)) & ~((uint64_t)PAGE_SIZE - 1);
                ov_end   = ov_end & ~((uint64_t)PAGE_SIZE - 1);

                for (uint64_t addr = ov_start; addr < ov_end; addr += PAGE_SIZE) {
                    uint64_t bit = addr / PAGE_SIZE;
                    if (bit < pmm_bitmap_pages) {
                        bitmap_clear((void*)PMM::bitmap, bit);
                    }
                }
            }
        }

        bitmap_set((void*)PMM::init_bitmap, l2_bit);
    }

    static inline void bitmap_set_sync(uint64_t bit) {
        uint64_t l2_bit = bit / L1_BITS_PER_L2_BIT;
        ensure_l2_init(l2_bit);

        bitmap_set((void*)PMM::bitmap, bit);

        uint64_t l1_base_idx64 = (l2_bit * L1_BITS_PER_L2_BIT) / 64;
        bool l2_full = true;

        for (int i = 0; i < (L1_BITS_PER_L2_BIT / 64); i++) {
            uint64_t idx = l1_base_idx64 + i;
            if (idx >= (bitmap_size / 8)) { l2_full = false; break; }
            if (((uint64_t*)PMM::bitmap)[idx] != 0xFFFFFFFFFFFFFFFF) { l2_full = false; break; }
        }

        if (l2_full && !bitmap_get((void*)PMM::bitmap_l2, l2_bit)) {
            bitmap_set((void*)PMM::bitmap_l2, l2_bit);

            uint64_t l3_bit = l2_bit / L2_BITS_PER_L3_BIT;
            uint64_t l2_base_idx64 = (l3_bit * L2_BITS_PER_L3_BIT) / 64;
            bool l3_full = true;

            for (int i = 0; i < (L2_BITS_PER_L3_BIT / 64); i++) {
                uint64_t idx = l2_base_idx64 + i;
                if (idx >= (bitmap_l2_size / 8)) { l3_full = false; break; }
                if (((uint64_t*)PMM::bitmap_l2)[idx] != 0xFFFFFFFFFFFFFFFF) { l3_full = false; break; }
            }

            if (l3_full) {
                bitmap_set((void*)PMM::bitmap_l3, l3_bit);
            }
        }
    }

    static inline void bitmap_clear_sync(uint64_t bit) {
        uint64_t l2_bit = bit / L1_BITS_PER_L2_BIT;
        ensure_l2_init(l2_bit);

        bitmap_clear((void*)PMM::bitmap, bit);

        if (bitmap_get((void*)PMM::bitmap_l2, l2_bit)) {
            bitmap_clear((void*)PMM::bitmap_l2, l2_bit);
            uint64_t l3_bit = l2_bit / L2_BITS_PER_L3_BIT;
            if (bitmap_get((void*)PMM::bitmap_l3, l3_bit)) {
                bitmap_clear((void*)PMM::bitmap_l3, l3_bit);
            }
        }
    }

    void Init() {
        pmm_memmap = memmap_request.response;
        uint64_t max_phys_addr = 0;

        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            uint64_t top = entry->base + entry->length;
            if (top > max_phys_addr) max_phys_addr = top;
        }

        pmm_bitmap_pages = max_phys_addr / PAGE_SIZE;

        uint64_t l1_bits = pmm_bitmap_pages;
        uint64_t l2_bits = ALIGN_UP(l1_bits, L1_BITS_PER_L2_BIT) / L1_BITS_PER_L2_BIT;
        uint64_t l3_bits = ALIGN_UP(l2_bits, L2_BITS_PER_L3_BIT) / L2_BITS_PER_L3_BIT;

        bitmap_size      = ALIGN_UP(l1_bits / 8, PAGE_SIZE);
        bitmap_l2_size   = ALIGN_UP(l2_bits / 8, PAGE_SIZE);
        bitmap_l3_size   = ALIGN_UP(l3_bits / 8, PAGE_SIZE);
        init_bitmap_size = ALIGN_UP(l2_bits / 8, PAGE_SIZE);

        uint64_t total_meta_size = bitmap_size + bitmap_l2_size + bitmap_l3_size + init_bitmap_size;

        bool meta_placed = false;
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];

            if (entry->type == LIMINE_MEMMAP_USABLE && entry->length >= total_meta_size) {
                PMM::bitmap      = (uint8_t*)HIGHER_HALF(entry->base);
                PMM::bitmap_l2   = (uint64_t*)((uintptr_t)PMM::bitmap + bitmap_size);
                PMM::bitmap_l3   = (uint64_t*)((uintptr_t)PMM::bitmap_l2 + bitmap_l2_size);
                PMM::init_bitmap = (uint8_t*)((uintptr_t)PMM::bitmap_l3 + bitmap_l3_size);

                // L1: everything starts as occupied; lazy init clears usable pages per 2MB block
                memset_fscpuf((void*)PMM::bitmap,      0xFF, bitmap_size);
                memset_fscpuf((void*)PMM::bitmap_l2,   0xFF, bitmap_l2_size);
                memset_fscpuf((void*)PMM::bitmap_l3,   0xFF, bitmap_l3_size);
                memset_fscpuf((void*)PMM::init_bitmap, 0x00, init_bitmap_size);

                entry->base   += total_meta_size;
                entry->length -= total_meta_size;

                meta_placed = true;
                break;
            }
        }

        if (!meta_placed) Panic("PMM: Failed to allocate metadata bitmap!");

        // --- Lazy occupancy marking: compute L2 at 2MB granularity from memmap ---
        uint64_t region_size_2mb = (uint64_t)L1_BITS_PER_L2_BIT * PAGE_SIZE;
        uint64_t total_l2_bits   = bitmap_l2_size * 8;
        uint64_t total_l3_bits   = bitmap_l3_size * 8;

        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->type != LIMINE_MEMMAP_USABLE) continue;

            uint64_t start_addr = entry->base;
            uint64_t end_addr   = entry->base + entry->length;
            if (end_addr == 0) continue;

            uint64_t start_l2 = start_addr / region_size_2mb;
            uint64_t end_l2   = (end_addr - 1) / region_size_2mb;

            for (uint64_t l2_bit = start_l2; l2_bit <= end_l2; l2_bit++) {
                if (l2_bit < total_l2_bits) {
                    bitmap_clear((void*)PMM::bitmap_l2, l2_bit);
                }
            }
        }

        // Compute L3 from L2: clear L3 bit if any L2 bit inside is 0 (not full)
        for (uint64_t l3_bit = 0; l3_bit < total_l3_bits; l3_bit++) {
            uint64_t l2_start = l3_bit * L2_BITS_PER_L3_BIT;
            uint64_t l2_end   = l2_start + L2_BITS_PER_L3_BIT;
            if (l2_end > total_l2_bits) l2_end = total_l2_bits;

            bool all_full = true;
            for (uint64_t b = l2_start; b < l2_end; b++) {
                if (!bitmap_get((void*)PMM::bitmap_l2, b)) { all_full = false; break; }
            }
            if (!all_full) {
                bitmap_clear((void*)PMM::bitmap_l3, l3_bit);
            }
        }

        bitmap_set_sync(0);
        pmm_bitmap_start = (uintptr_t)PMM::bitmap;
        pmm_bitmap_size  = bitmap_size;
        bitmap_last_free = 1;
    }

    void* GlobalRequestSingle() {
        uint64_t max_l1_idx64 = bitmap_size / 8;
        uint64_t max_l2_idx64 = bitmap_l2_size / 8;
        uint64_t max_l3_idx64 = bitmap_l3_size / 8;

        uint64_t start_l1_idx64 = bitmap_last_free / 64;
        uint64_t start_l2_idx64 = (bitmap_last_free / L1_BITS_PER_L2_BIT) / 64;
        uint64_t start_l3_idx64 = ((bitmap_last_free / L1_BITS_PER_L2_BIT) / L2_BITS_PER_L3_BIT) / 64;

        uint64_t cur_l1_start = start_l1_idx64;
        uint64_t cur_l2_start = start_l2_idx64;

        auto scan_l1 = [&](uint64_t l1_start, uint64_t l1_end) -> void* {
            for (uint64_t k = l1_start; k < l1_end && k < max_l1_idx64; k++) {
                // Lazy-init the L2 block that owns this 64-bit L1 slot
                uint64_t first_bit = k * 64;
                uint64_t l2_bit = first_bit / L1_BITS_PER_L2_BIT;
                ensure_l2_init(l2_bit);

                if (((uint64_t*)PMM::bitmap)[k] != 0xFFFFFFFFFFFFFFFF) {
                    uint64_t absolute_bit = k * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap)[k]);
                    if (absolute_bit >= pmm_bitmap_pages) return nullptr;
                    bitmap_set_sync(absolute_bit);
                    bitmap_last_free = absolute_bit + 1;
                    return (void*)(absolute_bit * PAGE_SIZE);
                }
            }
            return nullptr;
        };

        auto scan_l2 = [&](uint64_t l2_start, uint64_t l2_end) -> void* {
            for (uint64_t j = l2_start; j < l2_end && j < max_l2_idx64; j++) {
                if (((uint64_t*)PMM::bitmap_l2)[j] != 0xFFFFFFFFFFFFFFFF) {
                    uint64_t l2_bit = j * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap_l2)[j]);
                    uint64_t l1_base_idx64 = (l2_bit * L1_BITS_PER_L2_BIT) / 64;
                    uint64_t l1_end_idx64 = l1_base_idx64 + (L1_BITS_PER_L2_BIT / 64);
                    uint64_t l1_start = (j == l2_start) ? cur_l1_start : l1_base_idx64;
                    void* ret = scan_l1(l1_start, l1_end_idx64);
                    if (ret) return ret;
                    cur_l1_start = 0;
                }
            }
            return nullptr;
        };

        for (uint64_t i = start_l3_idx64; i < max_l3_idx64; i++) {
            if (((uint64_t*)PMM::bitmap_l3)[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t l3_bit = i * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap_l3)[i]);
                uint64_t l2_base_idx64 = (l3_bit * L2_BITS_PER_L3_BIT) / 64;
                uint64_t l2_end_idx64 = l2_base_idx64 + (L2_BITS_PER_L3_BIT / 64);
                uint64_t l2_start = (i == start_l3_idx64) ? cur_l2_start : l2_base_idx64;
                void* ret = scan_l2(l2_start, l2_end_idx64);
                if (ret) return ret;
                cur_l2_start = 0;
                cur_l1_start = 0;
            }
        }

        cur_l1_start = 0; cur_l2_start = 0;
        for (uint64_t i = 0; i <= start_l3_idx64 && i < max_l3_idx64; i++) {
            if (((uint64_t*)PMM::bitmap_l3)[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t l3_bit = i * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap_l3)[i]);
                uint64_t l2_base_idx64 = (l3_bit * L2_BITS_PER_L3_BIT) / 64;
                uint64_t l2_end_idx64 = l2_base_idx64 + (L2_BITS_PER_L3_BIT / 64);
                void* ret = scan_l2(l2_base_idx64, l2_end_idx64);
                if (ret) return ret;
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

        uint64_t l3_block_pages = L2_BITS_PER_L3_BIT * L1_BITS_PER_L2_BIT;
        uint64_t l2_block_pages = L1_BITS_PER_L2_BIT;

        while (true) {
            if (current_bit >= pmm_bitmap_pages) {
                if (wrapped) break;
                current_bit = 0; wrapped = true; free_count = 0; continue;
            }

            uint64_t l3_bit = (current_bit / L1_BITS_PER_L2_BIT) / L2_BITS_PER_L3_BIT;
            if (free_count == 0 && (current_bit % l3_block_pages == 0) && l3_bit < (bitmap_l3_size * 8)) {
                if (bitmap_get((void*)PMM::bitmap_l3, l3_bit)) { current_bit += l3_block_pages; continue; }
            }

            uint64_t l2_bit = current_bit / L1_BITS_PER_L2_BIT;
            if (free_count == 0 && (current_bit % l2_block_pages == 0) && l2_bit < (bitmap_l2_size * 8)) {
                if (bitmap_get((void*)PMM::bitmap_l2, l2_bit)) { current_bit += l2_block_pages; continue; }
            }

            // Ensure L1 region is lazily populated before touching L1 bits
            ensure_l2_init(l2_bit);

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

    // --- 2MB Page Allocation ---
    void* Request2MB() {
        spinlock_lock(&pmm_lock);
        uint64_t max_l2_idx64 = bitmap_l2_size / 8;

        for (uint64_t i = 0; i < max_l2_idx64; i++) {
            if (((uint64_t*)PMM::bitmap_l2)[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t bits = ~((uint64_t*)PMM::bitmap_l2)[i];
                while (bits) {
                    uint64_t l2_bit = i * 64 + __builtin_ctzll(bits);
                    uint64_t l1_start_bit = l2_bit * L1_BITS_PER_L2_BIT;
                    uint64_t l1_end_bit = l1_start_bit + L1_BITS_PER_L2_BIT;

                    if (l1_end_bit > pmm_bitmap_pages) {
                        bits &= bits - 1; continue;
                    }

                    // Lazy-init this 2MB block before inspecting L1
                    ensure_l2_init(l2_bit);

                    bool is_contiguous = true;
                    uint64_t start_idx64 = l1_start_bit / 64;
                    uint64_t end_idx64 = (l1_end_bit - 1) / 64;

                    for (uint64_t j = start_idx64; j <= end_idx64; j++) {
                        if (((uint64_t*)PMM::bitmap)[j] != 0) {
                            is_contiguous = false;
                            break;
                        }
                    }

                    if (is_contiguous) {
                        for (uint64_t j = l1_start_bit; j < l1_end_bit; j++) bitmap_set((void*)PMM::bitmap, j);
                        bitmap_set((void*)PMM::bitmap_l2, l2_bit);

                        uint64_t l3_bit = l2_bit / L2_BITS_PER_L3_BIT;
                        uint64_t l2_base_idx64_l3 = (l3_bit * L2_BITS_PER_L3_BIT) / 64;
                        bool l3_full = true;
                        for (uint64_t j = l2_base_idx64_l3; j < l2_base_idx64_l3 + (L2_BITS_PER_L3_BIT / 64); j++) {
                            if (j >= (bitmap_l2_size / 8) || ((uint64_t*)PMM::bitmap_l2)[j] != 0xFFFFFFFFFFFFFFFF) { l3_full = false; break; }
                        }
                        if (l3_full) bitmap_set((void*)PMM::bitmap_l3, l3_bit);

                        if (l1_start_bit < bitmap_last_free) bitmap_last_free = l1_start_bit;
                        spinlock_unlock(&pmm_lock);
                        return (void*)(l1_start_bit * PAGE_SIZE);
                    }
                    bits &= bits - 1;
                }
            }
        }
        spinlock_unlock(&pmm_lock);
        return nullptr;
    }

    // --- 2GB Page Allocation ---
    void* Request2GB() {
        spinlock_lock(&pmm_lock);
        uint64_t max_l3_idx64 = bitmap_l3_size / 8;

        for (uint64_t i = 0; i < max_l3_idx64; i++) {
            if (((uint64_t*)PMM::bitmap_l3)[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t bits = ~((uint64_t*)PMM::bitmap_l3)[i];
                while (bits) {
                    uint64_t l3_bit = i * 64 + __builtin_ctzll(bits);
                    uint64_t l2_start_bit = l3_bit * L2_BITS_PER_L3_BIT;
                    uint64_t l2_end_bit = l2_start_bit + L2_BITS_PER_L3_BIT;

                    if (l2_end_bit > (bitmap_l2_size * 8)) l2_end_bit = bitmap_l2_size * 8;

                    bool is_l2_clear = true;
                    uint64_t l2_start_idx64 = l2_start_bit / 64;
                    uint64_t l2_end_idx64 = (l2_end_bit - 1) / 64;
                    for (uint64_t j = l2_start_idx64; j <= l2_end_idx64; j++) {
                        if (((uint64_t*)PMM::bitmap_l2)[j] != 0) { is_l2_clear = false; break; }
                    }

                    if (is_l2_clear) {
                        uint64_t l1_start_bit = l2_start_bit * L1_BITS_PER_L2_BIT;
                        uint64_t l1_end_bit = l1_start_bit + (L2_BITS_PER_L3_BIT * L1_BITS_PER_L2_BIT);
                        if (l1_end_bit > pmm_bitmap_pages) l1_end_bit = pmm_bitmap_pages;

                        // Lazy-init every L2 block in this 2GB region
                        for (uint64_t l2b = l2_start_bit; l2b < l2_end_bit; l2b++) {
                            ensure_l2_init(l2b);
                        }

                        bool is_l1_clear = true;
                        uint64_t l1_start_idx64 = l1_start_bit / 64;
                        uint64_t l1_end_idx64 = (l1_end_bit - 1) / 64;
                        for (uint64_t j = l1_start_idx64; j <= l1_end_idx64; j++) {
                            if (((uint64_t*)PMM::bitmap)[j] != 0) { is_l1_clear = false; break; }
                        }

                        if (is_l1_clear) {
                            for (uint64_t j = l1_start_bit; j < l1_end_bit; j++) bitmap_set((void*)PMM::bitmap, j);
                            for (uint64_t j = l2_start_bit; j < l2_end_bit; j++) bitmap_set((void*)PMM::bitmap_l2, j);
                            bitmap_set((void*)PMM::bitmap_l3, l3_bit);

                            if (l1_start_bit < bitmap_last_free) bitmap_last_free = l1_start_bit;
                            spinlock_unlock(&pmm_lock);
                            return (void*)(l1_start_bit * PAGE_SIZE);
                        }
                    }
                    bits &= bits - 1;
                }
            }
        }
        spinlock_unlock(&pmm_lock);
        return nullptr;
    }

    void Free(void *ptr, uint64_t n = 1) {
        if (!ptr || n == 0) return;

        if (n == 1) {
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

    // --- 2MB Page Free ---
    void Free2MB(void* ptr) {
        if (!ptr) return;
        uint64_t l1_start_bit = (uint64_t)ptr / PAGE_SIZE;
        if (l1_start_bit % L1_BITS_PER_L2_BIT != 0) return;

        uint64_t l2_bit = l1_start_bit / L1_BITS_PER_L2_BIT;

        spinlock_lock(&pmm_lock);
        ensure_l2_init(l2_bit);

        uint64_t l1_end_bit = l1_start_bit + L1_BITS_PER_L2_BIT;
        for (uint64_t i = l1_start_bit; i < l1_end_bit; i++) bitmap_clear((void*)PMM::bitmap, i);

        bitmap_clear((void*)PMM::bitmap_l2, l2_bit);
        uint64_t l3_bit = l2_bit / L2_BITS_PER_L3_BIT;
        if (bitmap_get((void*)PMM::bitmap_l3, l3_bit)) {
            bitmap_clear((void*)PMM::bitmap_l3, l3_bit);
        }

        if (l1_start_bit < bitmap_last_free) bitmap_last_free = l1_start_bit;
        spinlock_unlock(&pmm_lock);
    }

    // --- 2GB Page Free ---
    void Free2GB(void* ptr) {
        if (!ptr) return;
        uint64_t l1_start_bit = (uint64_t)ptr / PAGE_SIZE;
        if (l1_start_bit % (L1_BITS_PER_L2_BIT * L2_BITS_PER_L3_BIT) != 0) return;

        uint64_t l2_start_bit = l1_start_bit / L1_BITS_PER_L2_BIT;
        uint64_t l3_bit = l2_start_bit / L2_BITS_PER_L3_BIT;

        spinlock_lock(&pmm_lock);
        uint64_t l1_end_bit = l1_start_bit + (L1_BITS_PER_L2_BIT * L2_BITS_PER_L3_BIT);
        uint64_t l2_end_bit = l2_start_bit + L2_BITS_PER_L3_BIT;

        for (uint64_t l2b = l2_start_bit; l2b < l2_end_bit; l2b++) {
            ensure_l2_init(l2b);
        }

        for (uint64_t i = l1_start_bit; i < l1_end_bit; i++) bitmap_clear((void*)PMM::bitmap, i);
        for (uint64_t i = l2_start_bit; i < l2_end_bit; i++) bitmap_clear((void*)PMM::bitmap_l2, i);
        bitmap_clear((void*)PMM::bitmap_l3, l3_bit);

        if (l1_start_bit < bitmap_last_free) bitmap_last_free = l1_start_bit;
        spinlock_unlock(&pmm_lock);
    }
}

#else
// =======================================================================
// Fallback Branch: General Architecture (No SMP/PCP, Pure L3 Bitmap)
// =======================================================================

namespace PMM {
    volatile uint8_t *bitmap = nullptr;
    volatile uint64_t *bitmap_l2 = nullptr;
    volatile uint64_t *bitmap_l3 = nullptr;
    volatile uint8_t *init_bitmap = nullptr;
    volatile uint64_t init_bitmap_size = 0;

    volatile uint64_t bitmap_size = 0;
    volatile uint64_t bitmap_l2_size = 0;
    volatile uint64_t bitmap_l3_size = 0;
    volatile uint64_t bitmap_last_free = 1;

    volatile uint64_t pmm_bitmap_start = 0;
    volatile uint64_t pmm_bitmap_size = 0;
    volatile uint64_t pmm_bitmap_pages = 0;

    static inline void ensure_l2_init(uint64_t l2_bit) {
        if (bitmap_get((void*)PMM::init_bitmap, l2_bit)) return;

        uint64_t l1_start = l2_bit * L1_BITS_PER_L2_BIT;
        uint64_t l1_end   = l1_start + L1_BITS_PER_L2_BIT;
        if (l1_end > pmm_bitmap_pages) l1_end = pmm_bitmap_pages;

        if (l1_start < pmm_bitmap_pages) {
            for (uint64_t i = l1_start; i < l1_end; i++) {
                bitmap_set((void*)PMM::bitmap, i);
            }

            uint64_t region_start_addr = l1_start * PAGE_SIZE;
            uint64_t region_end_addr   = l1_end   * PAGE_SIZE;

            for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
                struct limine_memmap_entry *entry = pmm_memmap->entries[i];
                if (entry->type != LIMINE_MEMMAP_USABLE) continue;

                uint64_t entry_start = entry->base;
                uint64_t entry_end   = entry->base + entry->length;
                if (entry_end <= region_start_addr || entry_start >= region_end_addr) continue;

                uint64_t ov_start = entry_start > region_start_addr ? entry_start : region_start_addr;
                uint64_t ov_end   = entry_end   < region_end_addr   ? entry_end   : region_end_addr;

                ov_start = (ov_start + (PAGE_SIZE - 1)) & ~((uint64_t)PAGE_SIZE - 1);
                ov_end   = ov_end & ~((uint64_t)PAGE_SIZE - 1);

                for (uint64_t addr = ov_start; addr < ov_end; addr += PAGE_SIZE) {
                    uint64_t bit = addr / PAGE_SIZE;
                    if (bit < pmm_bitmap_pages) {
                        bitmap_clear((void*)PMM::bitmap, bit);
                    }
                }
            }
        }

        bitmap_set((void*)PMM::init_bitmap, l2_bit);
    }

    static inline void bitmap_set_sync(uint64_t bit) {
        uint64_t l2_bit = bit / L1_BITS_PER_L2_BIT;
        ensure_l2_init(l2_bit);

        bitmap_set((void*)PMM::bitmap, bit);

        uint64_t l1_base_idx64 = (l2_bit * L1_BITS_PER_L2_BIT) / 64;
        bool l2_full = true;

        for (int i = 0; i < (L1_BITS_PER_L2_BIT / 64); i++) {
            uint64_t idx = l1_base_idx64 + i;
            if (idx >= (bitmap_size / 8)) { l2_full = false; break; }
            if (((uint64_t*)PMM::bitmap)[idx] != 0xFFFFFFFFFFFFFFFF) { l2_full = false; break; }
        }

        if (l2_full && !bitmap_get((void*)PMM::bitmap_l2, l2_bit)) {
            bitmap_set((void*)PMM::bitmap_l2, l2_bit);

            uint64_t l3_bit = l2_bit / L2_BITS_PER_L3_BIT;
            uint64_t l2_base_idx64 = (l3_bit * L2_BITS_PER_L3_BIT) / 64;
            bool l3_full = true;

            for (int i = 0; i < (L2_BITS_PER_L3_BIT / 64); i++) {
                uint64_t idx = l2_base_idx64 + i;
                if (idx >= (bitmap_l2_size / 8)) { l3_full = false; break; }
                if (((uint64_t*)PMM::bitmap_l2)[idx] != 0xFFFFFFFFFFFFFFFF) { l3_full = false; break; }
            }

            if (l3_full) {
                bitmap_set((void*)PMM::bitmap_l3, l3_bit);
            }
        }
    }

    static inline void bitmap_clear_sync(uint64_t bit) {
        uint64_t l2_bit = bit / L1_BITS_PER_L2_BIT;
        ensure_l2_init(l2_bit);

        bitmap_clear((void*)PMM::bitmap, bit);

        if (bitmap_get((void*)PMM::bitmap_l2, l2_bit)) {
            bitmap_clear((void*)PMM::bitmap_l2, l2_bit);
            uint64_t l3_bit = l2_bit / L2_BITS_PER_L3_BIT;
            if (bitmap_get((void*)PMM::bitmap_l3, l3_bit)) {
                bitmap_clear((void*)PMM::bitmap_l3, l3_bit);
            }
        }
    }

    void Init() {
        pmm_memmap = memmap_request.response;

        // Sanitize entry lengths (same behavior as before)
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length % PAGE_SIZE != 0) {
                kinfoln("ENTRY LENGTH: %d", entry->length);
                kwarn("Memory map entry length is NOT divisible by the page size. \n");
                entry->length = (entry->length / PAGE_SIZE) * PAGE_SIZE;
            }
        }

        // For lazy init we must index L1 by absolute page index (addr/PAGE_SIZE),
        // so pmm_bitmap_pages must be derived from max physical address.
        uint64_t max_phys_addr = 0;
        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            uint64_t top = entry->base + entry->length;
            if (top > max_phys_addr) max_phys_addr = top;
        }
        pmm_bitmap_pages = max_phys_addr / PAGE_SIZE;

        uint64_t l1_bits = pmm_bitmap_pages;
        uint64_t l2_bits = ALIGN_UP(l1_bits, L1_BITS_PER_L2_BIT) / L1_BITS_PER_L2_BIT;
        uint64_t l3_bits = ALIGN_UP(l2_bits, L2_BITS_PER_L3_BIT) / L2_BITS_PER_L3_BIT;

        bitmap_size      = ALIGN_UP(l1_bits / 8, PAGE_SIZE);
        bitmap_l2_size   = ALIGN_UP(l2_bits / 8, PAGE_SIZE);
        bitmap_l3_size   = ALIGN_UP(l3_bits / 8, PAGE_SIZE);
        init_bitmap_size = ALIGN_UP(l2_bits / 8, PAGE_SIZE);

        uint64_t total_meta_size = bitmap_size + bitmap_l2_size + bitmap_l3_size + init_bitmap_size;

        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->length >= total_meta_size && entry->type == LIMINE_MEMMAP_USABLE) {
                PMM::bitmap      = HIGHER_HALF((uint8_t*)entry->base);
                PMM::bitmap_l2   = (uint64_t*)((uint64_t)PMM::bitmap + bitmap_size);
                PMM::bitmap_l3   = (uint64_t*)((uint64_t)PMM::bitmap_l2 + bitmap_l2_size);
                PMM::init_bitmap = (uint8_t*)((uint64_t)PMM::bitmap_l3 + bitmap_l3_size);

                entry->length -= total_meta_size;
                entry->base   += total_meta_size;

                memset_fscpuf((void*)PMM::bitmap,      0xFF, bitmap_size);
                memset_fscpuf((void*)PMM::bitmap_l2,   0xFF, bitmap_l2_size);
                memset_fscpuf((void*)PMM::bitmap_l3,   0xFF, bitmap_l3_size);
                memset_fscpuf((void*)PMM::init_bitmap, 0x00, init_bitmap_size);
                break;
            }
        }

        // Lazy occupancy marking: compute L2 from memmap at 2MB granularity
        uint64_t region_size_2mb = (uint64_t)L1_BITS_PER_L2_BIT * PAGE_SIZE;
        uint64_t total_l2_bits   = bitmap_l2_size * 8;
        uint64_t total_l3_bits   = bitmap_l3_size * 8;

        for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
            struct limine_memmap_entry *entry = pmm_memmap->entries[i];
            if (entry->type != LIMINE_MEMMAP_USABLE) continue;

            uint64_t start_addr = entry->base;
            uint64_t end_addr   = entry->base + entry->length;
            if (end_addr == 0) continue;

            uint64_t start_l2 = start_addr / region_size_2mb;
            uint64_t end_l2   = (end_addr - 1) / region_size_2mb;

            for (uint64_t l2_bit = start_l2; l2_bit <= end_l2; l2_bit++) {
                if (l2_bit < total_l2_bits) {
                    bitmap_clear((void*)PMM::bitmap_l2, l2_bit);
                }
            }
        }

        // Compute L3 from L2
        for (uint64_t l3_bit = 0; l3_bit < total_l3_bits; l3_bit++) {
            uint64_t l2_start = l3_bit * L2_BITS_PER_L3_BIT;
            uint64_t l2_end   = l2_start + L2_BITS_PER_L3_BIT;
            if (l2_end > total_l2_bits) l2_end = total_l2_bits;

            bool all_full = true;
            for (uint64_t b = l2_start; b < l2_end; b++) {
                if (!bitmap_get((void*)PMM::bitmap_l2, b)) { all_full = false; break; }
            }
            if (!all_full) {
                bitmap_clear((void*)PMM::bitmap_l3, l3_bit);
            }
        }

        bitmap_set_sync(0);
        pmm_bitmap_start = (uint64_t)PMM::bitmap;
        pmm_bitmap_size  = bitmap_size;
        bitmap_last_free = 1;
    }

    static void* GlobalRequestSingle() {
        uint64_t max_l1_idx64 = bitmap_size / 8;
        uint64_t max_l2_idx64 = bitmap_l2_size / 8;
        uint64_t max_l3_idx64 = bitmap_l3_size / 8;

        uint64_t start_l1_idx64 = bitmap_last_free / 64;
        uint64_t start_l2_idx64 = (bitmap_last_free / L1_BITS_PER_L2_BIT) / 64;
        uint64_t start_l3_idx64 = ((bitmap_last_free / L1_BITS_PER_L2_BIT) / L2_BITS_PER_L3_BIT) / 64;

        uint64_t cur_l1_start = start_l1_idx64;
        uint64_t cur_l2_start = start_l2_idx64;

        auto scan_l1 = [&](uint64_t l1_start, uint64_t l1_end) -> void* {
            for (uint64_t k = l1_start; k < l1_end && k < max_l1_idx64; k++) {
                uint64_t first_bit = k * 64;
                uint64_t l2_bit = first_bit / L1_BITS_PER_L2_BIT;
                ensure_l2_init(l2_bit);

                if (((uint64_t*)PMM::bitmap)[k] != 0xFFFFFFFFFFFFFFFF) {
                    uint64_t absolute_bit = k * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap)[k]);
                    if (absolute_bit >= pmm_bitmap_pages) return nullptr;
                    bitmap_set_sync(absolute_bit);
                    bitmap_last_free = absolute_bit + 1;
                    return (void*)(absolute_bit * PAGE_SIZE);
                }
            }
            return nullptr;
        };

        auto scan_l2 = [&](uint64_t l2_start, uint64_t l2_end) -> void* {
            for (uint64_t j = l2_start; j < l2_end && j < max_l2_idx64; j++) {
                if (((uint64_t*)PMM::bitmap_l2)[j] != 0xFFFFFFFFFFFFFFFF) {
                    uint64_t l2_bit = j * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap_l2)[j]);
                    uint64_t l1_base_idx64 = (l2_bit * L1_BITS_PER_L2_BIT) / 64;
                    uint64_t l1_end_idx64 = l1_base_idx64 + (L1_BITS_PER_L2_BIT / 64);
                    uint64_t l1_start = (j == l2_start) ? cur_l1_start : l1_base_idx64;
                    void* ret = scan_l1(l1_start, l1_end_idx64);
                    if (ret) return ret;
                    cur_l1_start = 0;
                }
            }
            return nullptr;
        };

        for (uint64_t i = start_l3_idx64; i < max_l3_idx64; i++) {
            if (((uint64_t*)PMM::bitmap_l3)[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t l3_bit = i * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap_l3)[i]);
                uint64_t l2_base_idx64 = (l3_bit * L2_BITS_PER_L3_BIT) / 64;
                uint64_t l2_end_idx64 = l2_base_idx64 + (L2_BITS_PER_L3_BIT / 64);
                uint64_t l2_start = (i == start_l3_idx64) ? cur_l2_start : l2_base_idx64;
                void* ret = scan_l2(l2_start, l2_end_idx64);
                if (ret) return ret;
                cur_l2_start = 0;
                cur_l1_start = 0;
            }
        }

        cur_l1_start = 0; cur_l2_start = 0;
        for (uint64_t i = 0; i <= start_l3_idx64 && i < max_l3_idx64; i++) {
            if (((uint64_t*)PMM::bitmap_l3)[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t l3_bit = i * 64 + __builtin_ctzll(~((uint64_t*)PMM::bitmap_l3)[i]);
                uint64_t l2_base_idx64 = (l3_bit * L2_BITS_PER_L3_BIT) / 64;
                uint64_t l2_end_idx64 = l2_base_idx64 + (L2_BITS_PER_L3_BIT / 64);
                void* ret = scan_l2(l2_base_idx64, l2_end_idx64);
                if (ret) return ret;
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

        uint64_t l3_block_pages = L2_BITS_PER_L3_BIT * L1_BITS_PER_L2_BIT;
        uint64_t l2_block_pages = L1_BITS_PER_L2_BIT;

        while (true) {
            if (current_bit >= pmm_bitmap_pages) {
                if (wrapped) break;
                current_bit = 0; wrapped = true; free_count = 0; continue;
            }

            uint64_t l3_bit = (current_bit / L1_BITS_PER_L2_BIT) / L2_BITS_PER_L3_BIT;
            if (free_count == 0 && (current_bit % l3_block_pages == 0) && l3_bit < (bitmap_l3_size * 8)) {
                if (bitmap_get((void*)PMM::bitmap_l3, l3_bit)) { current_bit += l3_block_pages; continue; }
            }

            uint64_t l2_bit = current_bit / L1_BITS_PER_L2_BIT;
            if (free_count == 0 && (current_bit % l2_block_pages == 0) && l2_bit < (bitmap_l2_size * 8)) {
                if (bitmap_get((void*)PMM::bitmap_l2, l2_bit)) { current_bit += l2_block_pages; continue; }
            }

            ensure_l2_init(l2_bit);

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

    // --- 2MB Page Allocation ---
    void* Request2MB() {
        spinlock_lock(&pmm_lock);
        uint64_t max_l2_idx64 = bitmap_l2_size / 8;

        for (uint64_t i = 0; i < max_l2_idx64; i++) {
            if (((uint64_t*)PMM::bitmap_l2)[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t bits = ~((uint64_t*)PMM::bitmap_l2)[i];
                while (bits) {
                    uint64_t l2_bit = i * 64 + __builtin_ctzll(bits);
                    uint64_t l1_start_bit = l2_bit * L1_BITS_PER_L2_BIT;
                    uint64_t l1_end_bit = l1_start_bit + L1_BITS_PER_L2_BIT;

                    if (l1_end_bit > pmm_bitmap_pages) {
                        bits &= bits - 1; continue;
                    }

                    ensure_l2_init(l2_bit);

                    bool is_contiguous = true;
                    uint64_t start_idx64 = l1_start_bit / 64;
                    uint64_t end_idx64 = (l1_end_bit - 1) / 64;

                    for (uint64_t j = start_idx64; j <= end_idx64; j++) {
                        if (((uint64_t*)PMM::bitmap)[j] != 0) { is_contiguous = false; break; }
                    }

                    if (is_contiguous) {
                        for (uint64_t j = l1_start_bit; j < l1_end_bit; j++) bitmap_set((void*)PMM::bitmap, j);
                        bitmap_set((void*)PMM::bitmap_l2, l2_bit);

                        uint64_t l3_bit = l2_bit / L2_BITS_PER_L3_BIT;
                        uint64_t l2_base_idx64_l3 = (l3_bit * L2_BITS_PER_L3_BIT) / 64;
                        bool l3_full = true;
                        for (uint64_t j = l2_base_idx64_l3; j < l2_base_idx64_l3 + (L2_BITS_PER_L3_BIT / 64); j++) {
                            if (j >= (bitmap_l2_size / 8) || ((uint64_t*)PMM::bitmap_l2)[j] != 0xFFFFFFFFFFFFFFFF) { l3_full = false; break; }
                        }
                        if (l3_full) bitmap_set((void*)PMM::bitmap_l3, l3_bit);

                        if (l1_start_bit < bitmap_last_free) bitmap_last_free = l1_start_bit;
                        spinlock_unlock(&pmm_lock);
                        return (void*)(l1_start_bit * PAGE_SIZE);
                    }
                    bits &= bits - 1;
                }
            }
        }
        spinlock_unlock(&pmm_lock);
        return nullptr;
    }

    // --- 2GB Page Allocation ---
    void* Request2GB() {
        spinlock_lock(&pmm_lock);
        uint64_t max_l3_idx64 = bitmap_l3_size / 8;

        for (uint64_t i = 0; i < max_l3_idx64; i++) {
            if (((uint64_t*)PMM::bitmap_l3)[i] != 0xFFFFFFFFFFFFFFFF) {
                uint64_t bits = ~((uint64_t*)PMM::bitmap_l3)[i];
                while (bits) {
                    uint64_t l3_bit = i * 64 + __builtin_ctzll(bits);
                    uint64_t l2_start_bit = l3_bit * L2_BITS_PER_L3_BIT;
                    uint64_t l2_end_bit = l2_start_bit + L2_BITS_PER_L3_BIT;

                    if (l2_end_bit > (bitmap_l2_size * 8)) l2_end_bit = bitmap_l2_size * 8;

                    bool is_l2_clear = true;
                    uint64_t l2_start_idx64 = l2_start_bit / 64;
                    uint64_t l2_end_idx64 = (l2_end_bit - 1) / 64;
                    for (uint64_t j = l2_start_idx64; j <= l2_end_idx64; j++) {
                        if (((uint64_t*)PMM::bitmap_l2)[j] != 0) { is_l2_clear = false; break; }
                    }

                    if (is_l2_clear) {
                        uint64_t l1_start_bit = l2_start_bit * L1_BITS_PER_L2_BIT;
                        uint64_t l1_end_bit = l1_start_bit + (L2_BITS_PER_L3_BIT * L1_BITS_PER_L2_BIT);
                        if (l1_end_bit > pmm_bitmap_pages) l1_end_bit = pmm_bitmap_pages;

                        for (uint64_t l2b = l2_start_bit; l2b < l2_end_bit; l2b++) {
                            ensure_l2_init(l2b);
                        }

                        bool is_l1_clear = true;
                        uint64_t l1_start_idx64 = l1_start_bit / 64;
                        uint64_t l1_end_idx64 = (l1_end_bit - 1) / 64;
                        for (uint64_t j = l1_start_idx64; j <= l1_end_idx64; j++) {
                            if (((uint64_t*)PMM::bitmap)[j] != 0) { is_l1_clear = false; break; }
                        }

                        if (is_l1_clear) {
                            for (uint64_t j = l1_start_bit; j < l1_end_bit; j++) bitmap_set((void*)PMM::bitmap, j);
                            for (uint64_t j = l2_start_bit; j < l2_end_bit; j++) bitmap_set((void*)PMM::bitmap_l2, j);
                            bitmap_set((void*)PMM::bitmap_l3, l3_bit);

                            if (l1_start_bit < bitmap_last_free) bitmap_last_free = l1_start_bit;
                            spinlock_unlock(&pmm_lock);
                            return (void*)(l1_start_bit * PAGE_SIZE);
                        }
                    }
                    bits &= bits - 1;
                }
            }
        }
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

    // --- 2MB Page Free ---
    void Free2MB(void* ptr) {
        if (!ptr) return;
        uint64_t l1_start_bit = (uint64_t)ptr / PAGE_SIZE;
        if (l1_start_bit % L1_BITS_PER_L2_BIT != 0) return;

        uint64_t l2_bit = l1_start_bit / L1_BITS_PER_L2_BIT;

        spinlock_lock(&pmm_lock);
        ensure_l2_init(l2_bit);

        uint64_t l1_end_bit = l1_start_bit + L1_BITS_PER_L2_BIT;
        for (uint64_t i = l1_start_bit; i < l1_end_bit; i++) bitmap_clear((void*)PMM::bitmap, i);

        bitmap_clear((void*)PMM::bitmap_l2, l2_bit);
        uint64_t l3_bit = l2_bit / L2_BITS_PER_L3_BIT;
        if (bitmap_get((void*)PMM::bitmap_l3, l3_bit)) {
            bitmap_clear((void*)PMM::bitmap_l3, l3_bit);
        }

        if (l1_start_bit < bitmap_last_free) bitmap_last_free = l1_start_bit;
        spinlock_unlock(&pmm_lock);
    }

    // --- 2GB Page Free ---
    void Free2GB(void* ptr) {
        if (!ptr) return;
        uint64_t l1_start_bit = (uint64_t)ptr / PAGE_SIZE;
        if (l1_start_bit % (L1_BITS_PER_L2_BIT * L2_BITS_PER_L3_BIT) != 0) return;

        uint64_t l2_start_bit = l1_start_bit / L1_BITS_PER_L2_BIT;
        uint64_t l3_bit = l2_start_bit / L2_BITS_PER_L3_BIT;

        spinlock_lock(&pmm_lock);
        uint64_t l1_end_bit = l1_start_bit + (L1_BITS_PER_L2_BIT * L2_BITS_PER_L3_BIT);
        uint64_t l2_end_bit = l2_start_bit + L2_BITS_PER_L3_BIT;

        for (uint64_t l2b = l2_start_bit; l2b < l2_end_bit; l2b++) {
            ensure_l2_init(l2b);
        }

        for (uint64_t i = l1_start_bit; i < l1_end_bit; i++) bitmap_clear((void*)PMM::bitmap, i);
        for (uint64_t i = l2_start_bit; i < l2_end_bit; i++) bitmap_clear((void*)PMM::bitmap_l2, i);
        bitmap_clear((void*)PMM::bitmap_l3, l3_bit);

        if (l1_start_bit < bitmap_last_free) bitmap_last_free = l1_start_bit;
        spinlock_unlock(&pmm_lock);
    }
}
#endif