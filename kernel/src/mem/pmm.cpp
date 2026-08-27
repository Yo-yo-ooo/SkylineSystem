//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <mem/pmm.h>
#include <limine.h>
#include <klib/klib.h>

#if defined(__x86_64__)
  #include <arch/x86_64/smp/smp.h>
  #define PMM_HAS_PCP 1   // per-CPU single-page cache
#endif

static_assert(PAGE_SIZE == 4096, "PMM bitmap geometry assumes 4KiB pages");

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

// NOTE: `volatile` removed everywhere. Cross-core visibility is provided by
// the pmm_lock acquire/release barriers; volatile only defeated optimization.
spinlock_t pmm_lock = 0;
struct limine_memmap_response* pmm_memmap = nullptr;

namespace PMM {

// ---------------------------------------------------------------------------
// 3-level bitmap geometry
//   L1: 1 bit = 1 page (4KiB)   L2: 1 bit = 512 L1 bits (2MiB)
//   L3: 1 bit = 1024 L2 bits (2GiB)
// ---------------------------------------------------------------------------
static constexpr uint64_t L2_PAGES    = 512;                   // pages / L2 block
static constexpr uint64_t L2_PER_L3   = 1024;                  // L2 blocks / L3 block
static constexpr uint64_t L3_PAGES    = L2_PAGES * L2_PER_L3;  // pages / L3 block
static constexpr uint64_t L2_L1_WORDS = L2_PAGES / 64;         // 8
static constexpr uint64_t L3_L2_WORDS = L2_PER_L3 / 64;        // 16
static constexpr uint64_t L2_SIZE     = L2_PAGES * PAGE_SIZE;  // 2MiB
static constexpr uint64_t NO_BIT      = ~0ULL;

// All state below is only touched while holding pmm_lock (Init is single-threaded).
static uint64_t* l1_map;    // per-page occupancy, lazily populated per 2MiB block
static uint64_t* l2_map;    // L2 block full
static uint64_t* l3_map;    // L3 block full
static uint64_t* init_map;  // per-L2-block: L1 population done?

static uint64_t l1_map_size, l2_map_size, l3_map_size, init_map_size; // bytes
static uint64_t total_l2_bits, total_l3_bits;
static uint64_t bitmap_last_free = 1;

// Exported for the rest of the kernel (sync pmm.h: drop `volatile`).
uint64_t pmm_bitmap_pages = 0;
uint64_t pmm_bitmap_start = 0;
uint64_t pmm_bitmap_size  = 0;

// ---- 可观测性: 大块池健康度 ----
static uint64_t stat_req2m_ok    = 0;   // Request2MB 成功
static uint64_t stat_req2m_fail  = 0;   // Request2MB 失败
static uint64_t stat_req2gb_ok   = 0;
static uint64_t stat_req2gb_fail = 0;

// ---------------------------------------------------------------------------
// Word-level bitmap primitives
// ---------------------------------------------------------------------------
static inline bool bit_test(const uint64_t* map, uint64_t i) {
    return (map[i >> 6] >> (i & 63)) & 1ULL;
}
static inline void bit_set(uint64_t* map, uint64_t i) {
    map[i >> 6] |= 1ULL << (i & 63);
}
static inline void bit_clear(uint64_t* map, uint64_t i) {
    map[i >> 6] &= ~(1ULL << (i & 63));
}


void Stats(uint64_t* out /* [4] */) {
    IrqSpinGuard g(&pmm_lock);
    out[0] = stat_req2m_ok;    out[1] = stat_req2m_fail;
    out[2] = stat_req2gb_ok;   out[3] = stat_req2gb_fail;
}


static uint64_t free_pages = 0;

uint64_t FreePages() {
    return __atomic_load_n(&free_pages, __ATOMIC_RELAXED);   // 单 u64 松散读
}

// Clear / set bit range [first, last) in one pass over the covered words.
static inline void bits_clear(uint64_t* map, uint64_t first, uint64_t last) {
    if (first >= last) return;
    uint64_t wf = first >> 6, wl = (last - 1) >> 6;
    if (wf == wl) {
        uint64_t m = (~0ULL << (first & 63)) & (~0ULL >> (63 - ((last - 1) & 63)));
        map[wf] &= ~m;
        return;
    }
    map[wf] &= ~0ULL << (first & 63);
    for (uint64_t w = wf + 1; w < wl; w++) map[w] = 0;
    map[wl] &= ~(~0ULL >> (63 - ((last - 1) & 63)));
}
static inline void bits_set(uint64_t* map, uint64_t first, uint64_t last) {
    if (first >= last) return;
    uint64_t wf = first >> 6, wl = (last - 1) >> 6;
    if (wf == wl) {
        uint64_t m = (~0ULL << (first & 63)) & (~0ULL >> (63 - ((last - 1) & 63)));
        map[wf] |= m;
        return;
    }
    map[wf] |= ~0ULL << (first & 63);
    for (uint64_t w = wf + 1; w < wl; w++) map[w] = ~0ULL;
    map[wl] |= ~0ULL >> (63 - ((last - 1) & 63));
}

// ---------------------------------------------------------------------------
// Lazy L1 population: first touch of a 2MiB block defaults to "fully
// occupied", then clears pages the firmware reports USABLE (word-level).
// Idempotent. Caller holds pmm_lock (Init runs single-threaded).
// ---------------------------------------------------------------------------
static void ensure_l2_init(uint64_t l2_bit) {
    if (bit_test(init_map, l2_bit)) return;

    uint64_t wb = l2_bit * L2_L1_WORDS;
    for (uint64_t i = 0; i < L2_L1_WORDS; i++) l1_map[wb + i] = ~0ULL;

    uint64_t region_start = l2_bit * L2_SIZE;
    uint64_t region_end   = region_start + L2_SIZE;

    for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
        const struct limine_memmap_entry* e = pmm_memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;

        uint64_t e_start = e->base, e_end = e->base + e->length;
        if (e_end <= region_start || e_start >= region_end) continue;

        uint64_t s = e_start > region_start ? e_start : region_start;
        uint64_t t = e_end   < region_end   ? e_end   : region_end;
        s = (s + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        t &= ~(uint64_t)(PAGE_SIZE - 1);
        if (s < t) bits_clear(l1_map, s / PAGE_SIZE, t / PAGE_SIZE);
    }

    bit_set(init_map, l2_bit);
}


uint64_t VerifyFreeCount() {
    IrqSpinGuard g(&pmm_lock);
    uint64_t recount = 0;
    for (uint64_t l2 = 0; l2 < total_l2_bits; l2++) {
        if (bit_test(l2_map, l2)) continue;        /* 满 2MiB 块跳过 */
        ensure_l2_init(l2);
        uint64_t wb = l2 * L2_L1_WORDS;
        for (uint64_t w = 0; w < L2_L1_WORDS; w++)
            recount += __builtin_popcountll(~l1_map[wb + w]);
    }
    return recount > free_pages ? recount - free_pages : free_pages - recount;
}

// 2M 完整块余量: L2 位图 popcount (word 级)
uint64_t Free2MBlocks() {
    IrqSpinGuard g(&pmm_lock);
    uint64_t cnt = 0;
    for (uint64_t w = 0, words = l2_map_size / 8; w < words; w++)
        cnt += __builtin_popcountll(~l2_map[w]);
    return cnt;
}

// ---------------------------------------------------------------------------
// Locking: also block interrupts on x86_64 so an interrupt handler that
// allocates cannot deadlock against a lock holder on this CPU.
// ---------------------------------------------------------------------------
static inline void pmm_lock_acquire() {
#ifdef PMM_HAS_PCP
    Interrupt::Mask();
#endif
    spinlock_lock(&pmm_lock);
}
static inline void pmm_lock_release() {
    spinlock_unlock(&pmm_lock);
#ifdef PMM_HAS_PCP
    Interrupt::Unmask();
#endif
}




static inline bool l2_block_full(uint64_t l2_bit) {
    uint64_t wb = l2_bit * L2_L1_WORDS;
    for (uint64_t i = 0; i < L2_L1_WORDS; i++)
        if (l1_map[wb + i] != ~0ULL) return false;
    return true;
}

// Called after an L2 bit became 1: set the L3 bit if its block is now full.
static inline void l2_full_propagate(uint64_t l2_bit) {
    uint64_t l3_bit = l2_bit / L2_PER_L3;
    uint64_t wb = l3_bit * L3_L2_WORDS;
    for (uint64_t i = 0; i < L3_L2_WORDS; i++)
        if (l2_map[wb + i] != ~0ULL) return;
    bit_set(l3_map, l3_bit);
}

// ---------------------------------------------------------------------------
// Bulk transitions: maintain L1 + L2 + L3 together, one pass per 2MiB block.
// Caller holds pmm_lock. Requires start + n <= pmm_bitmap_pages.
// ---------------------------------------------------------------------------
static void mark_allocated(uint64_t start, uint64_t n) {
    uint64_t end = start + n;
    uint64_t first_l2 = start / L2_PAGES;
    uint64_t last_l2  = (end - 1) / L2_PAGES;

    for (uint64_t l2 = first_l2; l2 <= last_l2; l2++) {
        ensure_l2_init(l2);
        uint64_t b0 = (l2 == first_l2) ? start : l2 * L2_PAGES;
        uint64_t b1 = (l2 == last_l2)  ? end   : (l2 + 1) * L2_PAGES;
        bits_set(l1_map, b0, b1);
        if (l2_block_full(l2)) {
            bit_set(l2_map, l2);
            l2_full_propagate(l2);
        }
    }
}

static void mark_free(uint64_t start, uint64_t n) {
    uint64_t end = start + n;
    uint64_t first_l2 = start / L2_PAGES;
    uint64_t last_l2  = (end - 1) / L2_PAGES;

    for (uint64_t l2 = first_l2; l2 <= last_l2; l2++) {
        ensure_l2_init(l2);
        uint64_t b0 = (l2 == first_l2) ? start : l2 * L2_PAGES;
        uint64_t b1 = (l2 == last_l2)  ? end   : (l2 + 1) * L2_PAGES;
        bits_clear(l1_map, b0, b1);
        bit_clear(l2_map, l2);
        bit_clear(l3_map, l2 / L2_PER_L3);
    }
}

// ---------------------------------------------------------------------------
// Scanner: find n contiguous free pages in [from, to). Read-only.
// ---------------------------------------------------------------------------
static uint64_t scan_for_run(uint64_t from, uint64_t to, uint64_t n) {
    if (n == 0 || from >= to) return NO_BIT;

    uint64_t run = 0, run_start = 0;
    uint64_t bit = from;
    uint64_t inited_l2 = NO_BIT;

    while (bit < to) {
        if (run == 0) {
            if ((bit & (L3_PAGES - 1)) == 0) {            // 2GiB skip
                uint64_t l3 = bit / L3_PAGES;
                if (l3 < total_l3_bits && bit_test(l3_map, l3)) {
                    bit += L3_PAGES;
                    continue;
                }
            }
            if ((bit & (L2_PAGES - 1)) == 0) {            // 2MiB skip
                uint64_t l2 = bit / L2_PAGES;
                if (l2 < total_l2_bits && bit_test(l2_map, l2)) {
                    bit += L2_PAGES;
                    continue;
                }
            }
        }

        uint64_t cur_l2 = bit / L2_PAGES;
        if (cur_l2 != inited_l2) {
            ensure_l2_init(cur_l2);
            inited_l2 = cur_l2;
        }

        if ((bit & 63) == 0) {
            uint64_t w = l1_map[bit >> 6];
            if (run == 0) {
                if (w == ~0ULL) { bit += 64; continue; }
                uint64_t skip = __builtin_ctzll(~w);
                if (skip != 0) { bit += skip; continue; }
            } else if (w == 0) {
                uint64_t take = 64;
                if (run + take > n) take = n - run;
                if (bit + take > to) take = to - bit;
                run += take; bit += take;
                if (run == n) return run_start;
                continue;
            } else {
                uint64_t take = __builtin_ctzll(w);
                if (run + take > n) take = n - run;
                if (bit + take > to) take = to - bit;
                if (take != 0) {
                    run += take; bit += take;
                    if (run == n) return run_start;
                    continue;
                }
            }
        }

        if (!bit_test(l1_map, bit)) {
            if (run == 0) run_start = bit;
            if (++run == n) return run_start;
        } else {
            run = 0;
        }
        bit++;
    }
    return NO_BIT;
}

// Allocate n contiguous pages. Caller holds pmm_lock.
static void* alloc_pages_locked(uint64_t n) {
    uint64_t hint = bitmap_last_free;
    if (hint > pmm_bitmap_pages) hint = pmm_bitmap_pages;

    uint64_t bit = NO_BIT;
    if (hint < pmm_bitmap_pages)
        bit = scan_for_run(hint, pmm_bitmap_pages, n);
    if (bit == NO_BIT && hint > 0)
        bit = scan_for_run(0, hint, n);
    if (bit == NO_BIT) return nullptr;

    mark_allocated(bit, n);
    bitmap_last_free = bit + n;
    return (void*)(bit * PAGE_SIZE);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
void Init() {
    pmm_memmap = memmap_request.response;
    if (!pmm_memmap) Panic("PMM: Limine memmap response is NULL!");

    for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
        struct limine_memmap_entry* e = pmm_memmap->entries[i];
        if (e->length & (PAGE_SIZE - 1)) {
            kwarn("PMM: memmap entry #%lu not page-aligned (length=%lu), truncating\n",
                  (unsigned long)i, (unsigned long)e->length);
            e->length &= ~(uint64_t)(PAGE_SIZE - 1);
        }
    }

    uint64_t max_phys = 0;
    for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
        struct limine_memmap_entry* e = pmm_memmap->entries[i];
        uint64_t top = e->base + e->length;
        if (top > max_phys) max_phys = top;
    }
    if (max_phys == 0) Panic("PMM: empty memory map!");

    pmm_bitmap_pages = (max_phys + PAGE_SIZE - 1) / PAGE_SIZE;

    uint64_t l1_bits = pmm_bitmap_pages;
    total_l2_bits = (l1_bits + L2_PAGES - 1) / L2_PAGES;
    total_l3_bits = (total_l2_bits + L2_PER_L3 - 1) / L2_PER_L3;

    l1_map_size   = ALIGN_UP(total_l2_bits * L2_L1_WORDS * 8, PAGE_SIZE);
    l2_map_size   = ALIGN_UP(total_l3_bits * L3_L2_WORDS * 8, PAGE_SIZE);
    l3_map_size   = ALIGN_UP((total_l3_bits + 63) / 64 * 8, PAGE_SIZE);
    init_map_size = ALIGN_UP((total_l2_bits + 63) / 64 * 8, PAGE_SIZE);

    uint64_t total_meta = l1_map_size + l2_map_size + l3_map_size + init_map_size;

    bool placed = false;
    for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
        struct limine_memmap_entry* e = pmm_memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE || e->length < total_meta) continue;

        e->length -= total_meta;
        uint64_t meta_base = e->base + e->length;

        l1_map   = (uint64_t*)HIGHER_HALF(meta_base);
        l2_map   = (uint64_t*)((uintptr_t)l1_map + l1_map_size);
        l3_map   = (uint64_t*)((uintptr_t)l2_map + l2_map_size);
        init_map = (uint64_t*)((uintptr_t)l3_map + l3_map_size);

        memset_fscpuf(l1_map,   0xFF, l1_map_size);
        memset_fscpuf(l2_map,   0xFF, l2_map_size);
        memset_fscpuf(l3_map,   0xFF, l3_map_size);
        memset_fscpuf(init_map, 0x00, init_map_size);
        placed = true;
        break;
    }
    if (!placed) Panic("PMM: failed to place bitmap metadata!");

    for (uint64_t i = 0; i < pmm_memmap->entry_count; i++) {
        struct limine_memmap_entry* e = pmm_memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE || e->length == 0) continue;
        uint64_t end = e->base + e->length;
        if (end == 0) continue;
        bits_clear(l2_map, e->base / L2_SIZE, (end - 1) / L2_SIZE + 1);
    }

    for (uint64_t l3 = 0; l3 < total_l3_bits; l3++) {
        uint64_t wb = l3 * L3_L2_WORDS;
        for (uint64_t i = 0; i < L3_L2_WORDS; i++) {
            if (l2_map[wb + i] != ~0ULL) {
                bit_clear(l3_map, l3);
                break;
            }
        }
    }

    mark_allocated(0, 1);  // physical page 0 must never be handed out (NULL)

    
    free_pages = 0;
    for (uint64_t l2 = 0; l2 < total_l2_bits; l2++) {
        if (bit_test(l2_map, l2)) continue;        /* 满 2MiB 块: 0 空闲 */
        ensure_l2_init(l2);
        uint64_t wb = l2 * L2_L1_WORDS;
        for (uint64_t w = 0; w < L2_L1_WORDS; w++)
            free_pages += __builtin_popcountll(~l1_map[wb + w]);
    }
    /* 注: mark_allocated(0,1) 已在位图体现 → 计数自动少 1 ✓
       位图元数据页不在 USABLE 区 → 上扫不把它们计入 ✓ */

    pmm_bitmap_start = (uint64_t)l1_map;
    pmm_bitmap_size  = l1_map_size;
    bitmap_last_free = 1;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void* Request(uint64_t n) {
    if (n == 0 || n > pmm_bitmap_pages) return nullptr;

#ifdef PMM_HAS_PCP
    if (n == 1) {
        cpu_t* cpu = this_cpu();
        if (cpu) {
            IrqSave irq;                                // 只保护 per-CPU cache
            if (cpu->pmm_cache_count > 0)
                return cpu->pmm_cache[--cpu->pmm_cache_count];
        
            IrqSpinGuard g(&pmm_lock);
            void* page = alloc_pages_locked(1);
            if (page) {
                free_pages -= 1;                        
                uint32_t cached_before = cpu->pmm_cache_count;
                for (int i = 0; i < PMM_PCP_BATCH && cpu->pmm_cache_count < PMM_PCP_MAX; i++) {
                    void* extra = alloc_pages_locked(1);
                    if (!extra) break;
                    cpu->pmm_cache[cpu->pmm_cache_count++] = extra;
                }
                free_pages -= (uint64_t)(cpu->pmm_cache_count - cached_before);
            }
            return page;
        }
    }
#endif

    void* page;
    {
        IrqSpinGuard g(&pmm_lock);
        page = alloc_pages_locked(n);
        if (page) free_pages -= n;                      
    }
    if (!page)
        kerror("PMM: out of contiguous physical memory (%lu pages)\n", (unsigned long)n);
    return page;
}

void Free(void* ptr, uint64_t n) {
    if (!ptr || n == 0) return;

#ifdef PMM_HAS_PCP
    if (n == 1) {
        cpu_t* cpu = this_cpu();
        if (cpu) {
            IrqSave irq;
            if (cpu->pmm_cache_count < PMM_PCP_MAX) {
                cpu->pmm_cache[cpu->pmm_cache_count++] = ptr;
                return;                                
            }
            {   // cache 满：回吐一批到全局位图
                IrqSpinGuard g(&pmm_lock);
                uint64_t flushed = 0;
                for (int i = 0; i < PMM_PCP_BATCH && cpu->pmm_cache_count > 0; i++) {
                    uint64_t bit = (uint64_t)cpu->pmm_cache[--cpu->pmm_cache_count] / PAGE_SIZE;
                    mark_free(bit, 1);
                    if (bit < bitmap_last_free) bitmap_last_free = bit;
                    flushed++;
                }
                free_pages += flushed;                
            }
            cpu->pmm_cache[cpu->pmm_cache_count++] = ptr;
            return;                                     
        }
    }
#endif

    uint64_t start = (uint64_t)ptr / PAGE_SIZE;
    if (start >= pmm_bitmap_pages) return;
    if (n > pmm_bitmap_pages - start) n = pmm_bitmap_pages - start;

    IrqSpinGuard g(&pmm_lock);
    mark_free(start, n);
    free_pages += n;                                   
}

// --- 2MiB allocation ---
void* Request2MB() {
    IrqSpinGuard guard(&pmm_lock);

    for (uint64_t w = 0, words = l2_map_size / 8; w < words; w++) {
        uint64_t pending = ~l2_map[w];
        while (pending) {
            uint64_t l2_bit = w * 64 + __builtin_ctzll(pending);
            pending &= pending - 1;

            uint64_t start = l2_bit * L2_PAGES;
            if (start + L2_PAGES > pmm_bitmap_pages) continue;

            ensure_l2_init(l2_bit);

            uint64_t wb = l2_bit * L2_L1_WORDS;
            bool is_free = true;
            for (uint64_t i = 0; i < L2_L1_WORDS; i++) {
                if (l1_map[wb + i] != 0) { is_free = false; break; }
            }
            if (!is_free) continue;

            bits_set(l1_map, start, start + L2_PAGES);
            bit_set(l2_map, l2_bit);
            l2_full_propagate(l2_bit);

            if (start < bitmap_last_free) bitmap_last_free = start;
            free_pages -= L2_PAGES;                     
            stat_req2m_ok++;                            
            return (void*)(start * PAGE_SIZE);
        }
    }

    stat_req2m_fail++;
    return nullptr;
}

// --- 2GiB allocation ---
void* Request2GB() {
    IrqSpinGuard guard(&pmm_lock);

    for (uint64_t w = 0, words = l3_map_size / 8; w < words; w++) {
        uint64_t pending = ~l3_map[w];
        while (pending) {
            uint64_t l3_bit = w * 64 + __builtin_ctzll(pending);
            pending &= pending - 1;

            uint64_t start = l3_bit * L3_PAGES;
            if (start + L3_PAGES > pmm_bitmap_pages) continue;

            uint64_t wb = l3_bit * L3_L2_WORDS;
            bool l2_clear = true;
            for (uint64_t i = 0; i < L3_L2_WORDS; i++) {
                if (l2_map[wb + i] != 0) { l2_clear = false; break; }
            }
            if (!l2_clear) continue;

            uint64_t l2_first = l3_bit * L2_PER_L3;
            bool l1_clear = true;
            for (uint64_t l2 = l2_first; l2 < l2_first + L2_PER_L3 && l1_clear; l2++) {
                ensure_l2_init(l2);
                uint64_t w1 = l2 * L2_L1_WORDS;
                for (uint64_t i = 0; i < L2_L1_WORDS; i++) {
                    if (l1_map[w1 + i] != 0) { l1_clear = false; break; }
                }
            }
            if (!l1_clear) continue;

            bits_set(l1_map, start, start + L3_PAGES);
            bits_set(l2_map, l2_first, l2_first + L2_PER_L3);
            bit_set(l3_map, l3_bit);

            if (start < bitmap_last_free) bitmap_last_free = start;
            free_pages -= L3_PAGES;                  
            stat_req2gb_ok++;
            return (void*)(start * PAGE_SIZE);
        }
    }

    stat_req2gb_fail++;
    return nullptr;
}

void Free2MB(void* ptr) {
    if (!ptr) return;
    uint64_t start = (uint64_t)ptr / PAGE_SIZE;

    if (start & (L2_PAGES - 1)) return;
    if (start + L2_PAGES > pmm_bitmap_pages) return;

    IrqSpinGuard guard(&pmm_lock);
    ensure_l2_init(start / L2_PAGES);
    bits_clear(l1_map, start, start + L2_PAGES);
    bit_clear(l2_map, start / L2_PAGES);
    bit_clear(l3_map, start / L3_PAGES);
    if (start < bitmap_last_free) bitmap_last_free = start;
    free_pages += L2_PAGES;                           
}

void Free2GB(void* ptr) {
    if (!ptr) return;
    uint64_t start = (uint64_t)ptr / PAGE_SIZE;

    if (start & (L3_PAGES - 1)) return;
    if (start + L3_PAGES > pmm_bitmap_pages) return;

    uint64_t l2_first = start / L2_PAGES;
    IrqSpinGuard guard(&pmm_lock);
    for (uint64_t l2 = l2_first; l2 < l2_first + L2_PER_L3; l2++)
        ensure_l2_init(l2);
    bits_clear(l1_map, start, start + L3_PAGES);
    bits_clear(l2_map, l2_first, l2_first + L2_PER_L3);
    bit_clear(l3_map, start / L3_PAGES);
    if (start < bitmap_last_free) bitmap_last_free = start;
    free_pages += L3_PAGES;                             
}

} // namespace PMM