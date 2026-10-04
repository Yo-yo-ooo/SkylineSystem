//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <mem/pmm.h>
#include <limine.h>
#include <klib/klib.h>

#if defined(__x86_64__)
  #include <arch/x86_64/smp/smp.h>
  #include <arch/x86_64/schedule/sched.h>
  #define PMM_HAS_PCP 1   // per-CPU single-page cache
#endif

static_assert(PAGE_SIZE == 4096, "PMM bitmap geometry assumes 4KiB pages");

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0
};

// ---------------------------------------------------------------------------
// 分片锁总览
//   分片粒度 = 1 个 L3 块 (2GiB)。分片 si 独占:
//     l1_map   字 [si*8192, si*8192+8192)  (bit = 物理页)
//     l2_map   字 [si*16  , si*16+16)      (bit = 2MiB 块)
//     init_map 字 [si*16  , si*16+16)      (bit = L2 块)
//     l3_map   bit si
//   任意两片互不相交; 唯一跨片共享的是 l3_map 的"字"(一字含 64 片的
//   l3 位) → l3 单 bit 置/清用原子 RMW。
//
//   锁规则 (无死锁):
//     * 快路径至多持 1 把分片锁, 持锁期间不获取任何其他锁;
//     * 慢路径先取 pmm_lock, 再按下标升序取全部 PMM_NUM_LOCKS 把
//       分片锁 (每把只取一次); 快路径永不嵌套持锁 → 依赖图无环。
//     * 所有临界区关中断 (防止本 CPU 中断处理程序重入 PMM)。
//
//   ⚠ pmm_lock 不再串行化 PMM 常规操作: 内核其它处若通过
//   extern pmm_lock 与 PMM 互斥, 必须迁移 (见文末说明)。
// ---------------------------------------------------------------------------

spinlock_t pmm_lock = 0;   // 仅慢路径用于串行化 (保留符号兼容)
struct limine_memmap_response* pmm_memmap = nullptr;

namespace PMM {

// ---------------------------------------------------------------------------
// 3-level bitmap geometry
//   L1: 1 bit = 1 page (4KiB)   L2: 1 bit = 512 L1 bits (2MiB)
//   L3: 1 bit = 1024 L2 bits (2GiB)
// ---------------------------------------------------------------------------
static constexpr uint64_t L2_PAGES    = 512;
static constexpr uint64_t L2_PER_L3   = 1024;
static constexpr uint64_t L3_PAGES    = L2_PAGES * L2_PER_L3;
static constexpr uint64_t L2_L1_WORDS = L2_PAGES / 64;         // 8
static constexpr uint64_t L3_L2_WORDS = L2_PER_L3 / 64;        // 16
static constexpr uint64_t L2_SIZE     = L2_PAGES * PAGE_SIZE;  // 2MiB
static constexpr uint64_t NO_BIT      = ~0ULL;

// ---- 分片 ----
static constexpr uint64_t SHARD_PAGES   = L3_PAGES;   // 2GiB / 分片
static constexpr uint64_t PMM_NUM_LOCKS = 32;         // 2 的幂, 可调
static_assert((PMM_NUM_LOCKS & (PMM_NUM_LOCKS - 1)) == 0, "power of two");
static_assert(L2_PER_L3 == L3_L2_WORDS * 64, "shard must own whole l2_map words");
static_assert(L2_PAGES  == L2_L1_WORDS * 64, "shard must own whole l1_map words");

static uint64_t* l1_map;    // per-page occupancy, lazily populated per 2MiB block
static uint64_t* l2_map;    // L2 block full
static uint64_t* l3_map;    // L3 block full (单 bit 原子更新, 见下)
static uint64_t* init_map;  // per-L2-block: L1 population done?

static uint64_t l1_map_size, l2_map_size, l3_map_size, init_map_size;
static uint64_t total_l2_bits, total_l3_bits;

// Exported for the rest of the kernel.
uint64_t pmm_bitmap_pages = 0;
uint64_t pmm_bitmap_start = 0;
uint64_t pmm_bitmap_size  = 0;

// ---- 可观测性: 分片化后在不同锁下更新 → 原子 ----
static uint64_t stat_req2m_ok   = 0;
static uint64_t stat_req2m_fail = 0;
static uint64_t stat_req2gb_ok  = 0;
static uint64_t stat_req2gb_fail = 0;
static inline void stat_inc(uint64_t* s) {
    __atomic_fetch_add(s, 1, __ATOMIC_RELAXED);
}

// ---------------------------------------------------------------------------
// 分片描述符与锁
//   free_pages: 持本片锁时为精确值 (与位图同临界区更新); 锁外只做
//   RELAXED 原子读, 作为"跳片"启发 —— 可能读到旧值: 只会错过刚被
//   释放的页 (回退到下一片/慢路径), 不会错误分配。
// ---------------------------------------------------------------------------
struct pmm_shard {
    uint64_t first_page;    // = si * SHARD_PAGES
    uint64_t npages;        // 末片可能不足 2GiB
    uint64_t hint;          // 本片 last-free 提示 (持锁读写)
    uint64_t free_pages;    // 本片空闲页数 (见上)
} __attribute__((aligned(64)));          // 64B 对齐防跨片伪共享

struct pmm_shardlock {
    spinlock_t lock;
} __attribute__((aligned(64)));
static_assert(sizeof(spinlock_t) <= 64, "unexpected spinlock size");

static pmm_shardlock shard_locks[PMM_NUM_LOCKS];   // BSS: 0 = 未锁
static pmm_shard*    shards      = nullptr;
static uint64_t      shard_count = 0;

static uint64_t rr_seq = 0;     // 起始分片轮转, 让各 CPU 的请求自然散开

static uint64_t free_pages = 0; // 全局总数 (O(1) 松散读; 更新为原子 RMW)

static inline spinlock_t* shard_lock(uint64_t si) {
    return &shard_locks[si & (PMM_NUM_LOCKS - 1)].lock;
}
// 计数必须与位图变更处于同一临界区 (调用方保证)。
static inline void shard_free_add(uint64_t si, uint64_t n) {
    __atomic_fetch_add(&shards[si].free_pages, n, __ATOMIC_RELAXED);
    __atomic_fetch_add(&free_pages, n, __ATOMIC_RELAXED);
}
static inline void shard_free_sub(uint64_t si, uint64_t n) {
    __atomic_fetch_sub(&shards[si].free_pages, n, __ATOMIC_RELAXED);
    __atomic_fetch_sub(&free_pages, n, __ATOMIC_RELAXED);
}
static inline uint64_t shard_rr_first() {
    return __atomic_fetch_add(&rr_seq, 1, __ATOMIC_RELAXED) % shard_count;
}

uint64_t FreePages() {
    return __atomic_load_n(&free_pages, __ATOMIC_RELAXED);   // 单 u64 松散读
}

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
// l3_map 专用: 一个字被 64 个分片共享 → 原子 RMW。
// RELAXED 足够: 每片对自己那一位的视图由本片锁保证一致,
// 原子性只为避免同字内丢更新。
static inline void bit_set_atomic(uint64_t* map, uint64_t i) {
    __atomic_fetch_or(&map[i >> 6], 1ULL << (i & 63), __ATOMIC_RELAXED);
}
static inline void bit_clear_atomic(uint64_t* map, uint64_t i) {
    __atomic_fetch_and(&map[i >> 6], ~(1ULL << (i & 63)), __ATOMIC_RELAXED);
}

void Stats(uint64_t* out /* [4] */) {
    out[0] = __atomic_load_n(&stat_req2m_ok,   __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&stat_req2m_fail, __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&stat_req2gb_ok,  __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&stat_req2gb_fail,__ATOMIC_RELAXED);
}

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
// Lazy L1 population. Idempotent. 读 memmap (Init 后只读)。
// 调用者需持有该 L2 块所属分片的锁 (或全部分片锁)。
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

static inline bool l2_block_full(uint64_t l2_bit) {
    uint64_t wb = l2_bit * L2_L1_WORDS;
    for (uint64_t i = 0; i < L2_L1_WORDS; i++)
        if (l1_map[wb + i] != ~0ULL) return false;
    return true;
}

// L2 块变满后向上传播。l3 位跨片共享 → 原子置位。
static inline void l2_full_propagate(uint64_t l2_bit) {
    uint64_t l3_bit = l2_bit / L2_PER_L3;
    uint64_t wb = l3_bit * L3_L2_WORDS;
    for (uint64_t i = 0; i < L3_L2_WORDS; i++)
        if (l2_map[wb + i] != ~0ULL) return;
    bit_set_atomic(l3_map, l3_bit);
}

// ---------------------------------------------------------------------------
// Bulk transitions: L1+L2+L3 一起维护, 按 2MiB 块一趟。
// 调用者需持有覆盖 [start, start+n) 的分片锁 (跨片时为全部分片锁)。
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
        bit_clear_atomic(l3_map, l2 / L2_PER_L3);   // 跨片共享字 → 原子
    }
}

// ---------------------------------------------------------------------------
// Scanner: find n contiguous free pages in [from, to). Read-only.
// [from, to) 必须落在调用者已持锁的范围内 (单片或全部)。
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

// ---------------------------------------------------------------------------
// 单片操作: 调用者持有分片 si 的锁
// ---------------------------------------------------------------------------
// 在分片 si 内分配 n 连续页 (n <= npages)。不更新计数 —— 由调用者在
// 同一临界区内调用 shard_free_sub()。
static void* alloc_in_shard_locked(uint64_t si, uint64_t n) {
    pmm_shard* s = &shards[si];
    if (s->free_pages < n) return nullptr;            // 锁内精确

    const uint64_t base = s->first_page;
    const uint64_t end  = base + s->npages;
    uint64_t hint = s->hint;
    if (hint < base || hint > end) hint = base;

    uint64_t bit = scan_for_run(hint, end, n);
    if (bit == NO_BIT && hint > base)
        bit = scan_for_run(base, hint, n);
    if (bit == NO_BIT) return nullptr;

    mark_allocated(bit, n);
    s->hint = (bit + n >= end) ? base : (bit + n);
    return (void*)(bit * PAGE_SIZE);
}

// 释放分片 si 内 [start, start+n)。调用者持有该分片的锁。
static void free_in_shard_locked(uint64_t si, uint64_t start, uint64_t n) {
    mark_free(start, n);
    pmm_shard* s = &shards[si];
    if (start < s->hint) s->hint = start;
    shard_free_add(si, n);
}

// ---------------------------------------------------------------------------
// 慢路径守卫: 关中断+取 pmm_lock, 再按下标升序取全部分片锁。
// (若裸原语名不是 spinlock_lock/spinlock_unlock, 请同步修改。)
// ---------------------------------------------------------------------------
struct AllShardsGuard {
    IrqSpinGuard gate;                     // pmm_lock: 串行化慢路径
    AllShardsGuard() : gate(&pmm_lock) {
        for (unsigned i = 0; i < PMM_NUM_LOCKS; i++)
            spinlock_lock(&shard_locks[i].lock);
    }
    ~AllShardsGuard() {
        for (unsigned i = PMM_NUM_LOCKS; i-- > 0; )
            spinlock_unlock(&shard_locks[i].lock);
    }
    AllShardsGuard(const AllShardsGuard&) = delete;
    AllShardsGuard& operator=(const AllShardsGuard&) = delete;
};

static uint64_t slow_hint = 1;             // 仅在持有全部分片锁时读写

// 跨片连续区分配: n > 2GiB, 或所有单片尝试失败后的兜底。
// 语义与旧单锁版本一致 (全位图范围查找, 允许跨 2GiB 边界的连续区)。
static void* alloc_global_slowpath(uint64_t n) {
    AllShardsGuard all;

    uint64_t hint = slow_hint;
    if (hint > pmm_bitmap_pages) hint = pmm_bitmap_pages;
    uint64_t bit = NO_BIT;
    if (hint < pmm_bitmap_pages)
        bit = scan_for_run(hint, pmm_bitmap_pages, n);
    if (bit == NO_BIT && hint > 0)
        bit = scan_for_run(0, hint, n);
    if (bit == NO_BIT) return nullptr;

    mark_allocated(bit, n);
    slow_hint = bit + n;

    // 计数按覆盖的分片分别扣除
    const uint64_t e = bit + n;
    for (uint64_t si = bit / SHARD_PAGES; si <= (e - 1) / SHARD_PAGES; si++) {
        uint64_t lo = bit > si * SHARD_PAGES ? bit : si * SHARD_PAGES;
        uint64_t hi = e < (si + 1) * SHARD_PAGES ? e : (si + 1) * SHARD_PAGES;
        shard_free_sub(si, hi - lo);
    }
    return (void*)(bit * PAGE_SIZE);
}

#ifdef PMM_HAS_PCP
// per-CPU 缓存批量补充: 逐片尝试, 每片至多一把锁。
// 成功: 返回 1 页, 其余 (<= PMM_PCP_BATCH) 填入 cache。
// 失败: 所有分片瞬时均无空闲页。
static void* pcp_refill(cpu_t* cpu) {
    uint64_t first = shard_rr_first();
    for (uint64_t t = 0; t < shard_count; t++) {
        uint64_t si = first + t;
        if (si >= shard_count) si -= shard_count;
        if (__atomic_load_n(&shards[si].free_pages, __ATOMIC_RELAXED) == 0)
            continue;                       // 锁外启发式跳片

        IrqSpinGuard g(shard_lock(si));
        void* page = alloc_in_shard_locked(si, 1);
        if (!page) continue;

        uint32_t before = cpu->pmm_cache_count;
        for (int i = 0; i < PMM_PCP_BATCH && cpu->pmm_cache_count < PMM_PCP_MAX; i++) {
            void* extra = alloc_in_shard_locked(si, 1);
            if (!extra) break;
            cpu->pmm_cache[cpu->pmm_cache_count++] = extra;
        }
        shard_free_sub(si, 1 + (uint64_t)(cpu->pmm_cache_count - before));
        return page;
    }
    return nullptr;
}
#endif

// ---------------------------------------------------------------------------
// Init (单线程, SMP 启动前)
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
    shard_count   = total_l3_bits;

    l1_map_size   = ALIGN_UP(total_l2_bits * L2_L1_WORDS * 8, PAGE_SIZE);
    l2_map_size   = ALIGN_UP(total_l3_bits * L3_L2_WORDS * 8, PAGE_SIZE);
    l3_map_size   = ALIGN_UP((total_l3_bits + 63) / 64 * 8, PAGE_SIZE);
    init_map_size = ALIGN_UP((total_l2_bits + 63) / 64 * 8, PAGE_SIZE);
    uint64_t shards_size = ALIGN_UP(shard_count * sizeof(pmm_shard), PAGE_SIZE);

    uint64_t total_meta = l1_map_size + l2_map_size + l3_map_size
                        + init_map_size + shards_size;

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
        shards   = (pmm_shard*)((uintptr_t)init_map + init_map_size);  // 页对齐 → 满足 64B 对齐

        memset_fscpuf(l1_map,   0xFF, l1_map_size);
        memset_fscpuf(l2_map,   0xFF, l2_map_size);
        memset_fscpuf(l3_map,   0xFF, l3_map_size);
        memset_fscpuf(init_map, 0x00, init_map_size);
        memset_fscpuf(shards,   0x00, shards_size);
        placed = true;
        break;
    }
    if (!placed) Panic("PMM: failed to place bitmap metadata!");

    for (uint64_t si = 0; si < shard_count; si++) {
        shards[si].first_page = si * SHARD_PAGES;
        uint64_t np = pmm_bitmap_pages - shards[si].first_page;
        shards[si].npages     = np < SHARD_PAGES ? np : SHARD_PAGES;
        shards[si].hint       = shards[si].first_page;
        shards[si].free_pages = 0;
    }

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
                bit_clear(l3_map, l3);      // Init 单线程, 普通操作即可
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
        uint64_t cnt = 0;
        for (uint64_t w = 0; w < L2_L1_WORDS; w++)
            cnt += __builtin_popcountll(~l1_map[wb + w]);
        shards[l2 / L2_PER_L3].free_pages += cnt;  // Init 单线程
        free_pages += cnt;
    }

    pmm_bitmap_start = (uint64_t)l1_map;
    pmm_bitmap_size  = l1_map_size;
    slow_hint = 1;
    rr_seq    = 0;
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

            void* page = pcp_refill(cpu);               // 分片化批量补充
            if (page) return page;
            // 各分片瞬时均无空闲 → 落到通用路径做最终兜底
        }
    }
#endif

    // 快路径: 单片内分配, 至多一把分片锁
    if (n <= SHARD_PAGES) {
        uint64_t first = shard_rr_first();
        for (uint64_t t = 0; t < shard_count; t++) {
            uint64_t si = first + t;
            if (si >= shard_count) si -= shard_count;
            if (__atomic_load_n(&shards[si].free_pages, __ATOMIC_RELAXED) < n)
                continue;                               // 锁外启发式跳片

            IrqSpinGuard g(shard_lock(si));
            void* page = alloc_in_shard_locked(si, n);
            if (page) {
                shard_free_sub(si, n);
                return page;
            }
        }
        // 全部单片失败 → 慢路径兜底 (允许跨 2GiB 边界的连续区)
    }

    // 慢路径: n > 2GiB 或上述兜底; 全锁按序, 语义与旧版一致
    void* page = alloc_global_slowpath(n);
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
            {   // cache 满: 回吐一批到各自分片 (每片单锁)
                for (int i = 0; i < PMM_PCP_BATCH && cpu->pmm_cache_count > 0; i++) {
                    uint64_t bit = (uint64_t)cpu->pmm_cache[--cpu->pmm_cache_count] / PAGE_SIZE;
                    if (bit >= pmm_bitmap_pages) continue;       // 防御
                    uint64_t si = bit / SHARD_PAGES;
                    IrqSpinGuard g(shard_lock(si));
                    free_in_shard_locked(si, bit, 1);
                }
            }
            cpu->pmm_cache[cpu->pmm_cache_count++] = ptr;
            return;
        }
    }
#endif

    uint64_t start = (uint64_t)ptr / PAGE_SIZE;
    if (start >= pmm_bitmap_pages) return;
    if (n > pmm_bitmap_pages - start) n = pmm_bitmap_pages - start;
    uint64_t end = start + n;

    // 按分片切段释放: 每段一把锁 (段间独立, 无需同时持多把锁)
    uint64_t si = start / SHARD_PAGES;
    for (;;) {
        uint64_t lo = start > si * SHARD_PAGES ? start : si * SHARD_PAGES;
        uint64_t hi = end < (si + 1) * SHARD_PAGES ? end : (si + 1) * SHARD_PAGES;
        IrqSpinGuard g(shard_lock(si));
        free_in_shard_locked(si, lo, hi - lo);
        if (hi >= end) break;
        si++;
    }
}

// --- 2MiB allocation --- (整块位于单一分片内 → 单锁)
void* Request2MB() {
    uint64_t first = shard_rr_first();
    for (uint64_t t = 0; t < shard_count; t++) {
        uint64_t si = first + t;
        if (si >= shard_count) si -= shard_count;
        if (__atomic_load_n(&shards[si].free_pages, __ATOMIC_RELAXED) < L2_PAGES)
            continue;

        IrqSpinGuard g(shard_lock(si));

        const uint64_t wbase = si * L3_L2_WORDS;        // 本片独占的 16 个 l2 word
        for (uint64_t w = 0; w < L3_L2_WORDS; w++) {
            uint64_t pending = ~l2_map[wbase + w];
            while (pending) {
                uint64_t l2_bit = (wbase + w) * 64 + __builtin_ctzll(pending);
                pending &= pending - 1;

                uint64_t start = l2_bit * L2_PAGES;
                if (start + L2_PAGES > pmm_bitmap_pages) continue;   // 尾部残块

                ensure_l2_init(l2_bit);
                uint64_t wb = l2_bit * L2_L1_WORDS;
                bool is_free = true;
                for (uint64_t i = 0; i < L2_L1_WORDS; i++)
                    if (l1_map[wb + i] != 0) { is_free = false; break; }
                if (!is_free) continue;

                bits_set(l1_map, start, start + L2_PAGES);
                bit_set(l2_map, l2_bit);
                l2_full_propagate(l2_bit);

                if (start < shards[si].hint) shards[si].hint = start;
                shard_free_sub(si, L2_PAGES);
                stat_inc(&stat_req2m_ok);
                return (void*)(start * PAGE_SIZE);
            }
        }
    }
    stat_inc(&stat_req2m_fail);
    return nullptr;
}

// --- 2GiB allocation --- (恰为一个分片 → 单锁)
void* Request2GB() {
    uint64_t first = shard_rr_first();
    for (uint64_t t = 0; t < shard_count; t++) {
        uint64_t si = first + t;
        if (si >= shard_count) si -= shard_count;
        if (shards[si].npages < L3_PAGES) continue;     // 精确: 末片不足 2GiB
        if (__atomic_load_n(&shards[si].free_pages, __ATOMIC_RELAXED) < L3_PAGES)
            continue;

        IrqSpinGuard g(shard_lock(si));

        const uint64_t wb = si * L3_L2_WORDS;
        bool l2_clear = true;
        for (uint64_t i = 0; i < L3_L2_WORDS; i++)
            if (l2_map[wb + i] != 0) { l2_clear = false; break; }
        if (!l2_clear) continue;

        const uint64_t l2_first = si * L2_PER_L3;
        bool l1_clear = true;
        for (uint64_t l2 = l2_first; l2 < l2_first + L2_PER_L3 && l1_clear; l2++) {
            ensure_l2_init(l2);
            uint64_t w1 = l2 * L2_L1_WORDS;
            for (uint64_t i = 0; i < L2_L1_WORDS; i++)
                if (l1_map[w1 + i] != 0) { l1_clear = false; break; }
        }
        if (!l1_clear) continue;

        const uint64_t start = si * L3_PAGES;
        bits_set(l1_map, start, start + L3_PAGES);
        bits_set(l2_map, l2_first, l2_first + L2_PER_L3);
        bit_set_atomic(l3_map, si);

        if (start < shards[si].hint) shards[si].hint = start;
        shard_free_sub(si, L3_PAGES);
        stat_inc(&stat_req2gb_ok);
        return (void*)(start * PAGE_SIZE);
    }
    stat_inc(&stat_req2gb_fail);
    return nullptr;
}

void Free2MB(void* ptr) {
    if (!ptr) return;
    uint64_t start = (uint64_t)ptr / PAGE_SIZE;
    if (start & (L2_PAGES - 1)) return;
    if (start + L2_PAGES > pmm_bitmap_pages) return;

    uint64_t si = start / SHARD_PAGES;
    IrqSpinGuard g(shard_lock(si));
    free_in_shard_locked(si, start, L2_PAGES);
}

void Free2GB(void* ptr) {
    if (!ptr) return;
    uint64_t start = (uint64_t)ptr / PAGE_SIZE;
    if (start & (L3_PAGES - 1)) return;
    if (start + L3_PAGES > pmm_bitmap_pages) return;

    uint64_t si = start / SHARD_PAGES;
    IrqSpinGuard g(shard_lock(si));
    free_in_shard_locked(si, start, L3_PAGES);
}

// 全部锁: 与旧版一样得到全局一致快照
uint64_t VerifyFreeCount() {
    AllShardsGuard all;
    uint64_t recount = 0;
    for (uint64_t l2 = 0; l2 < total_l2_bits; l2++) {
        if (bit_test(l2_map, l2)) continue;        /* 满 2MiB 块跳过 */
        ensure_l2_init(l2);
        uint64_t wb = l2 * L2_L1_WORDS;
        for (uint64_t w = 0; w < L2_L1_WORDS; w++)
            recount += __builtin_popcountll(~l1_map[wb + w]);
    }
    uint64_t cur = __atomic_load_n(&free_pages, __ATOMIC_RELAXED);
    return recount > cur ? recount - cur : cur - recount;
}

// 逐片快照汇总 (诊断用, 允许片间不同瞬间)
uint64_t Free2MBlocks() {
    uint64_t cnt = 0;
    for (uint64_t si = 0; si < shard_count; si++) {
        IrqSpinGuard g(shard_lock(si));
        uint64_t wb = si * L3_L2_WORDS;
        for (uint64_t i = 0; i < L3_L2_WORDS; i++)
            cnt += __builtin_popcountll(~l2_map[wb + i]);
    }
    return cnt;
}

// ---- 可观测性: 分片健康度 (可选; 需在 pmm.h 加声明) ----
uint64_t ShardCount() { return shard_count; }
void ShardFreePages(uint64_t* out, uint64_t max) {
    uint64_t cnt = shard_count < max ? shard_count : max;
    for (uint64_t si = 0; si < cnt; si++)
        out[si] = __atomic_load_n(&shards[si].free_pages, __ATOMIC_RELAXED);
}

} // namespace PMM