//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#include <private/alloc/alloc.h>
#include <stdc/string.h>
#ifdef __x86_64__
#include <base/arch/x86_64/atomic/common.h>
#include <base/arch/x86_64/syscall.h>
#endif
#include <stdc/stdlib.h>
#include <base/base.h>

#define PTF(x) syscall(24, (long)x, sizeof(x), 0, 0, 0, 0);
//#define PTF(x)

extern volatile uint64_t SizeClassTable[75][3];

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

#define ALLOC_MAX_RETRIES       10000
#define SCB_INIT_SPIN_TIMEOUT   10000

#define LARGE_OBJ_CACHE_MAX     4

/*
 * epoch 差 × 队列深度 组合豁免
 *
 *   豁免条件:  (gc_generation - slot.last_epoch) * pending_depth >= BUDGET
 *
 *   - pending_depth 小 (内存压力低): 需要极大的 epoch_gap 才豁免 → 保守,
 *     几乎不会误豁免捏着旧指针的线程;
 *   - pending_depth 大 (积压严重): 较小的 epoch_gap 即豁免 → 激进,
 *     用微小 UAF 风险换回内存 (工程折衷);
 *   - BUDGET 量纲: epoch × SCB 个数。
 *     参考: 队列深 16 (一个 flush 批次) 时, 128 代不活跃即豁免
 *     → 16*128 = 2048。
 */
#define QSBR_DEFER_BUDGET       2048ULL
/* 硬性下限: 无论压力多大, epoch_gap 低于此值绝不豁免 —— 保证活跃线程
   (哪怕低频) 永远不会被豁免掉 */
#define QSBR_MIN_EPOCH_GAP      8ULL

#define DIV_ROUND_UP(x, y) (((x) + ((y) - 1)) / (y))
#define PAGE_SIZE 4096

#define IS_LARGE_PATH_FLAG      0x80000000U

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
    sys_munmap((uint64_t)x, y);
}

void* MoreCore(uint64_t PageCount) {
    uint64_t p = sys_mmap(0, PageCount, 2, 0, 0);
    return (p != 0 && p != (uint64_t)-1ULL) ? (void*)p : NULL;
}

// ============================================================================
// QSBR 子系统 & GC 分代队列
// ============================================================================

typedef struct {
    volatile uint64_t count;
    volatile uint64_t last_epoch;
    char padding[48];
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
static volatile uint64_t global_qsbr_slot_alloc = 0;

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
    void* _fl_head = (void*)(tls_list); \
    void* _fl_tail = _fl_head; \
    while (*(void**)_fl_tail) _fl_tail = *(void**)_fl_tail; \
    void* _fl_old = NULL; \
    do { \
        _fl_old = (void*)(global_list); \
        *(void**)_fl_tail = _fl_old; \
        atomic_thread_fence(ATOMIC_RELEASE); \
    } while (__a_cas_p(&(global_list), _fl_old, _fl_head) != _fl_old); \
    atomic_fetch_add_n(&(global_count), (tls_count), ATOMIC_RELEASE); \
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
            AllocBlock_t* header = (AllocBlock_t*)p;
            header->AllocSizeAligned = SizeClassTable[i][0];
            _free_large_object_real(p, header);
            p = next;
        }
        tls_large_cache[i] = NULL;
        tls_large_cache_cnt[i] = 0;
    }
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
        tls_data.qsbr_slot = atomic_fetch_add_n(&global_qsbr_slot_alloc, 1, ATOMIC_RELAXED) % QSBR_SLOTS;
    }
    atomic_fetch_add_n(&qsbr_counters[tls_data.qsbr_slot].count, 1, ATOMIC_ACQUIRE);
    atomic_store_n(&qsbr_counters[tls_data.qsbr_slot].last_epoch,
                   atomic_load_n(&gc_generation, ATOMIC_ACQUIRE), ATOMIC_RELEASE);
    return tls_data.qsbr_slot;
}

static inline void qsbr_leave(int32_t slot) {
    atomic_sub_fetch_n(&qsbr_counters[slot].count, 1, ATOMIC_RELEASE);
}

/* ============================================================================
 *
 * pending_depth: 本轮待回收的 SCB 总数 (调用方统计后传入)。
 *   GC 成功的收益正比于它, 豁免的风险随它上升 —— 乘进判据正好对冲。
 *
 * 判定流程 (每个 slot):
 *   1. 从未分配的 slot → 静默 (break)
 *   2. epoch_gap < QSBR_MIN_EPOCH_GAP → 必须走经典 count 前进判定
 *      (硬下限: 短期不活跃绝不豁免)
 *   3. epoch_gap >= MIN 且 gap × depth >= BUDGET → 豁免候选:
 *      双读校验 —— 重读 count, 与首读比较。两次读之间 count 变了,
 *      说明线程刚刚活动过, 撤回豁免改走经典判定 (它会通过, 因为
 *      count 已前进)。
 *   4. 其余 → 经典判定: SEQ_CST fence 后重读 count, 未前进 = 不静默
 * ==========================================================================*/
static inline int32_t is_quiescent(uint64_t pending_depth) {
    uint64_t generation = atomic_load_n(&gc_generation, ATOMIC_ACQUIRE);
    uint64_t slots_in_use = atomic_load_n(&global_qsbr_slot_alloc, ATOMIC_RELAXED);

    for (int32_t i = 0; i < QSBR_SLOTS; i++) {
        if ((uint64_t)i >= slots_in_use) break;

        uint64_t before = atomic_load_n(&qsbr_counters[i].count, ATOMIC_ACQUIRE);
        uint64_t last   = atomic_load_n(&qsbr_counters[i].last_epoch, ATOMIC_ACQUIRE);
        uint64_t gap    = generation - last;

        /* 组合豁免: 时间 × 压力超预算, 且过硬性时间下限 */
        if (gap >= QSBR_MIN_EPOCH_GAP && gap * pending_depth >= QSBR_DEFER_BUDGET) {
            /* 双读校验: 豁免前重读 count —— 两次读之间线程动了就撤回 */
            uint64_t recheck = atomic_load_n(&qsbr_counters[i].count, ATOMIC_ACQUIRE);
            if (recheck != before) {
                /* 线程刚刚活跃: count 已前进, 经典判定会放行, 走下面 */
            } else {
                continue;   /* 维持豁免: 视为静默 */
            }
        }

        /* 经典判定: 要求 count 前进 (线程跨越了静默点) */
        atomic_thread_fence(ATOMIC_SEQ_CST);
        uint64_t after = atomic_load_n(&qsbr_counters[i].count, ATOMIC_ACQUIRE);
        if (after <= before)
            return 0;
    }
    return 1;
}

static inline void try_gc() {
    uint32_t expected = 0;
    if (!atomic_compare_exchange_n(&gc_lock, &expected, 1, 0, ATOMIC_ACQUIRE, ATOMIC_RELAXED)) {
        return;
    }

    flush_tls_scb_all();

    void* small_list = (void*)(uintptr_t)atomic_exchange_n(&old_small_scb_list, NULL, ATOMIC_ACQ_REL);
    void* large_list = (void*)(uintptr_t)atomic_exchange_n(&old_large_scb_list, NULL, ATOMIC_ACQ_REL);
    void* young_small = (void*)(uintptr_t)atomic_exchange_n(&deferred_small_scb_list, NULL, ATOMIC_ACQ_REL);
    void* young_large = (void*)(uintptr_t)atomic_exchange_n(&deferred_large_scb_list, NULL, ATOMIC_ACQ_REL);

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

    uint64_t n_small = 0, n_large = 0;
    for (void* p = small_list; p; p = *(void**)p) n_small++;
    for (void* p = large_list; p; p = *(void**)p) n_large++;

    /* 队列深度传入 is_quiescent 参与豁免判据 */
    int32_t gc_success = is_quiescent(n_small + n_large);

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

        atomic_store_n(&pending_small_count, 0, ATOMIC_RELEASE);
        atomic_store_n(&pending_large_count, 0, ATOMIC_RELEASE);
        atomic_fetch_add_n(&gc_generation, 1, ATOMIC_RELEASE);
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
        atomic_fetch_add_n(&pending_small_count, n_small, ATOMIC_RELEASE);
        atomic_fetch_add_n(&pending_large_count, n_large, ATOMIC_RELEASE);
    }

unlock:
    atomic_store_n(&gc_lock, 0, ATOMIC_RELEASE);
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

    if (region_size != 0) {
        uint64_t step_size = size_class + sizeof(AllocBlock_t);
        uint64_t total_objects = region_size / step_size;
        if (total_objects == 0 || total_objects > SCB_MAX_OBJECTS) {
            region_size = 0;
        }
    }

    int32_t alloc_retries = 0;

    if (region_size == 0 && tls_large_cache_cnt[idx] > 0) {
        AllocBlock_t* header = (AllocBlock_t*)tls_large_cache[idx];
        tls_large_cache[idx] = *(void**)header;
        tls_large_cache_cnt[idx]--;
        header->AllocSizeAligned = size_class;
        header->AllocSizeUnAligned = size;
        header->BitMapBitLocation.MCBBitLocation |= IS_LARGE_PATH_FLAG;
        return (void*)header->AllocPtrBaseAddress;
    }

    while (!allocated_ptr) {
        if (++alloc_retries > ALLOC_MAX_RETRIES) {
            try_gc();
            return NULL;
        }
        if (alloc_retries == ALLOC_MAX_RETRIES / 2) {
            try_gc();
        }

        MainControlBlock_t* mcb = NULL;
        MainControlBlock_t* prev_mcb = NULL;

        for (int32_t retry = 0; retry < 10; retry++) {
            mcb = (MainControlBlock_t*)SizeClassTable[idx][1];
            prev_mcb = NULL;
            int32_t walk_limit = 2048;

            while (mcb && walk_limit--) {
                __builtin_prefetch(mcb, 0, 3);
                if (atomic_load_n(&mcb->is_full, ATOMIC_ACQUIRE) == 0) {
                    if (prev_mcb == NULL || atomic_load_n(&prev_mcb->next, ATOMIC_ACQUIRE) == (uint64_t)mcb) {
                        break;
                    }
                }
                prev_mcb = mcb;
                mcb = (MainControlBlock_t*)atomic_load_n(&mcb->next, ATOMIC_ACQUIRE);
            }
            if (mcb && walk_limit >= 0) break;
            CPU_RELAX();
        }

        if (!mcb) {
            MainControlBlock_t* new_mcb = (MainControlBlock_t*)MoreCore(4);
            if (!new_mcb) { try_gc(); continue; }

            new_mcb->is_full = 0;
            new_mcb->rem_scb_count = MCB_SCB_COUNT;
            for (int32_t i = 0; i < MCB_BITMAP_WORDS; i++) new_mcb->bitmap[i] = 0;
            uint64_t tail_bits = MCB_BITMAP_WORDS * 64 - MCB_SCB_COUNT;
            new_mcb->bitmap[MCB_BITMAP_WORDS - 1] = 0xFFFFFFFFFFFFFFFFULL << (64 - tail_bits);
            for (int32_t i = 0; i < MCB_SCB_COUNT; i++) new_mcb->list_base[i] = 0;
            new_mcb->next = 0;

            void* actual = (prev_mcb == NULL)
                ? __a_cas_p((volatile void*)&SizeClassTable[idx][1], NULL, new_mcb)
                : __a_cas_p((volatile void*)&prev_mcb->next, NULL, new_mcb);

            if (actual != NULL) {
                LessCore(new_mcb, 4);
                try_gc();
                continue;
            }
            mcb = new_mcb;
        }

        uint64_t scb_idx = 0xFFFFFFFFFFFFFFFFULL;
        for (int32_t i = 0; i < MCB_BITMAP_WORDS; i++) {
            uint64_t current_bitmap = atomic_load_n(&mcb->bitmap[i], ATOMIC_ACQUIRE);
            if (current_bitmap != 0xFFFFFFFFFFFFFFFFULL) {
                scb_idx = (uint64_t)i * 64 + __builtin_ctzll(~current_bitmap);
                break;
            }
        }

        if (scb_idx >= MCB_SCB_COUNT) {
            atomic_store_n(&mcb->is_full, 1, ATOMIC_RELEASE);
            continue;
        }

        // ==================== 小对象路径 ====================
        if (region_size != 0) {
            SecondControlBlock_t* scb = (SecondControlBlock_t*)atomic_load_n(&mcb->list_base[scb_idx], ATOMIC_ACQUIRE);
            __builtin_prefetch(scb, 0, 3);

            if (!scb) {
                void* expected = NULL;
                if (atomic_compare_exchange_n(&mcb->list_base[scb_idx], &expected, (void*)1, 0, ATOMIC_ACQ_REL, ATOMIC_RELAXED)) {
                    uint64_t step_size = size_class + sizeof(AllocBlock_t);
                    uint64_t total_objects = region_size / step_size;

                    if (total_objects == 0 || total_objects > SCB_MAX_OBJECTS) {
                        uint64_t mask = 1ULL << (scb_idx % 64);
                        __a_or_64(&mcb->bitmap[scb_idx / 64], mask);
                        __a_subu32(&mcb->rem_scb_count, 1);
                        atomic_store_n(&mcb->list_base[scb_idx], NULL, ATOMIC_RELEASE);
                        continue;
                    }

                    scb = (SecondControlBlock_t*)MoreCore(4);
                    if (!scb) {
                        atomic_store_n(&mcb->list_base[scb_idx], NULL, ATOMIC_RELEASE);
                        try_gc();
                        continue;
                    }
                    memset(scb, 0, sizeof(SecondControlBlock_t));

                    uint64_t data_pages = DIV_ROUND_UP(total_objects * step_size, PAGE_SIZE);
                    void* data_region = MoreCore(data_pages);
                    if (!data_region) {
                        LessCore(scb, 4);
                        atomic_store_n(&mcb->list_base[scb_idx], NULL, ATOMIC_RELEASE);
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

                    atomic_store_n(&mcb->list_base[scb_idx], scb, ATOMIC_RELEASE);
                } else {
                    uint32_t spin_cnt = 0;
                    while ((scb = (SecondControlBlock_t*)atomic_load_n(&mcb->list_base[scb_idx], ATOMIC_ACQUIRE)) == (void*)1) {
                        CPU_RELAX();
                        if (++spin_cnt > SCB_INIT_SPIN_TIMEOUT) {
                            void* cmp = (void*)1;
                            atomic_compare_exchange_n(&mcb->list_base[scb_idx], &cmp, NULL, 0, ATOMIC_ACQ_REL, ATOMIC_RELAXED);
                            break;
                        }
                    }
                    if (!scb || scb == (void*)1) continue;
                }
            }

            uint64_t scb_max_words = (scb->bit_tail / 64) + 1;
            if (scb_max_words > SCB_BITMAP_WORDS)
                scb_max_words = SCB_BITMAP_WORDS;
            uint64_t obj_idx = 0xFFFFFFFFFFFFFFFFULL;

            for (uint64_t i = 0; i < scb_max_words; i++) {
                int32_t cas_retry = 0;
                while (1) {
                    if (++cas_retry > 1000) break;

                    uint64_t current_bitmap = atomic_load_n(&scb->bitmap[i], ATOMIC_ACQUIRE);
                    if (current_bitmap == 0xFFFFFFFFFFFFFFFFULL) break;

                    int32_t free_bit = __builtin_ctzll(~current_bitmap);
                    uint64_t current_bit = i * 64 + free_bit;
                    if (current_bit > scb->bit_tail) break;

                    uint64_t new_bitmap = current_bitmap | (1ULL << free_bit);
                    if (A_CAS_U64(&scb->bitmap[i], current_bitmap, new_bitmap) == current_bitmap) {
                        if (atomic_load_n((uint64_t*)&mcb->list_base[scb_idx], ATOMIC_ACQUIRE) != (uint64_t)scb) {
                            uint64_t rb_old, rb_new;
                            do {
                                rb_old = scb->bitmap[i];
                                rb_new = rb_old & ~(1ULL << free_bit);
                            } while (A_CAS_U64(&scb->bitmap[i], rb_old, rb_new) != rb_old);
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
                uint64_t mask = 1ULL << (scb_idx % 64);
                uint64_t old_val = atomic_fetch_or_n(&mcb->bitmap[scb_idx / 64], mask, ATOMIC_RELAXED);
                if (!(old_val & mask)) {
                    if (__a_subu32(&mcb->rem_scb_count, 1) == 1) {
                        atomic_store_n(&mcb->is_full, 1, ATOMIC_RELEASE);
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
            header->BitMapBitLocation.MCBBitLocation = scb_idx & ~IS_LARGE_PATH_FLAG;
            header->BitMapBase           = (uint64_t)scb;
            header->MCBAddr              = (uint64_t)mcb;

            allocated_ptr = (void*)header->AllocPtrBaseAddress;

            if (__a_subu32(&scb->rem_count, 1) == 1) {
                uint64_t expected = 0;
                if (atomic_compare_exchange_n(&scb->is_full, &expected, 1, 0, ATOMIC_ACQ_REL, ATOMIC_RELAXED)) {
                    __a_or_64(&mcb->bitmap[scb_idx / 64], (1ULL << (scb_idx % 64)));
                    if (__a_subu32(&mcb->rem_scb_count, 1) == 1) {
                        atomic_store_n(&mcb->is_full, 1, ATOMIC_RELEASE);
                    }
                }
            }
        }
        // ==================== 大对象路径 ====================
        else {
            LargeSecondControlBlock_t* l_scb = (LargeSecondControlBlock_t*)atomic_load_n(&mcb->list_base[scb_idx], ATOMIC_ACQUIRE);

            if (!l_scb) {
                void* expected = NULL;
                if (atomic_compare_exchange_n(&mcb->list_base[scb_idx], &expected, (void*)1, 0, ATOMIC_ACQ_REL, ATOMIC_RELAXED)) {
                    l_scb = (LargeSecondControlBlock_t*)MoreCore(4);
                    if (!l_scb) {
                        atomic_store_n(&mcb->list_base[scb_idx], NULL, ATOMIC_RELEASE);
                        try_gc();
                        continue;
                    }

                    l_scb->is_full = 0;
                    l_scb->rem_count = MCB_SCB_COUNT;
                    for (int32_t i = 0; i < MCB_BITMAP_WORDS; i++) l_scb->bitmap[i] = 0;
                    uint64_t tail_bits = MCB_BITMAP_WORDS * 64 - MCB_SCB_COUNT;
                    l_scb->bitmap[MCB_BITMAP_WORDS - 1] = 0xFFFFFFFFFFFFFFFFULL << (64 - tail_bits);
                    for (int32_t i = 0; i < MCB_SCB_COUNT; i++) l_scb->list_base[i] = 0;

                    atomic_store_n(&mcb->list_base[scb_idx], l_scb, ATOMIC_RELEASE);
                } else {
                    uint32_t spin_cnt = 0;
                    while ((l_scb = (LargeSecondControlBlock_t*)atomic_load_n(&mcb->list_base[scb_idx], ATOMIC_ACQUIRE)) == (void*)1) {
                        CPU_RELAX();
                        if (++spin_cnt > SCB_INIT_SPIN_TIMEOUT) {
                            void* cmp = (void*)1;
                            atomic_compare_exchange_n(&mcb->list_base[scb_idx], &cmp, NULL, 0, ATOMIC_ACQ_REL, ATOMIC_RELAXED);
                            break;
                        }
                    }
                    if (!l_scb || l_scb == (void*)1) continue;
                }
            }

            uint64_t obj_idx = 0xFFFFFFFFFFFFFFFFULL;
            for (int32_t i = 0; i < MCB_BITMAP_WORDS; i++) {
                int32_t cas_retry = 0;
                while (1) {
                    if (++cas_retry > 1000) break;

                    uint64_t current_bitmap = atomic_load_n(&l_scb->bitmap[i], ATOMIC_ACQUIRE);
                    if (current_bitmap == 0xFFFFFFFFFFFFFFFFULL) break;

                    int32_t free_bit = __builtin_ctzll(~current_bitmap);
                    uint64_t current_bit = (uint64_t)i * 64 + free_bit;
                    if (current_bit >= MCB_SCB_COUNT) break;

                    uint64_t new_bitmap = current_bitmap | (1ULL << free_bit);
                    if (A_CAS_U64(&l_scb->bitmap[i], current_bitmap, new_bitmap) == current_bitmap) {
                        if (atomic_load_n((uint64_t*)&mcb->list_base[scb_idx], ATOMIC_ACQUIRE) != (uint64_t)l_scb) {
                            uint64_t rb_old, rb_new;
                            do {
                                rb_old = l_scb->bitmap[i];
                                rb_new = rb_old & ~(1ULL << free_bit);
                            } while (A_CAS_U64(&l_scb->bitmap[i], rb_old, rb_new) != rb_old);
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
                uint64_t mask = 1ULL << (scb_idx % 64);
                uint64_t old_val = atomic_fetch_or_n(&mcb->bitmap[scb_idx / 64], mask, ATOMIC_RELAXED);
                if (!(old_val & mask)) {
                    if (__a_subu32(&mcb->rem_scb_count, 1) == 1) {
                        atomic_store_n(&mcb->is_full, 1, ATOMIC_RELEASE);
                    }
                }
                continue;
            }

            uint64_t total_large_size = size_class + sizeof(AllocBlock_t);
            void* page_start = MoreCore(DIV_ROUND_UP(total_large_size, PAGE_SIZE));
            if (!page_start) {
                uint64_t mask = 1ULL << (obj_idx % 64);
                if (atomic_load_n(&mcb->list_base[scb_idx], ATOMIC_ACQUIRE) == (uint64_t)l_scb) {
                    __a_clear_bit(&l_scb->bitmap[obj_idx / 64], mask);
                }
                try_gc();
                continue;
            }

            AllocBlock_t* header = (AllocBlock_t*)page_start;
            header->AllocSizeAligned     = size_class;
            header->AllocSizeUnAligned   = size;
            header->BlockAddr            = (uint64_t)page_start;
            header->AllocPtrBaseAddress  = (uint64_t)page_start + sizeof(AllocBlock_t);
            header->Magic                = ALLOC_BLOCK_MAGIC;
            header->BitMapBitLocation.SCBBitLocation = obj_idx;
            header->BitMapBitLocation.MCBBitLocation = scb_idx | IS_LARGE_PATH_FLAG;
            header->BitMapBase           = (uint64_t)l_scb;
            header->MCBAddr              = (uint64_t)mcb;

            allocated_ptr = (void*)header->AllocPtrBaseAddress;
            l_scb->list_base[obj_idx] = (uint64_t)page_start;

            if (__a_subu64(&l_scb->rem_count, 1) == 1) {
                uint64_t expected = 0;
                if (atomic_compare_exchange_n(&l_scb->is_full, &expected, 1, 0, ATOMIC_ACQ_REL, ATOMIC_RELAXED)) {
                    __a_or_64(&mcb->bitmap[scb_idx / 64], (1ULL << (scb_idx % 64)));
                    if (__a_subu32(&mcb->rem_scb_count, 1) == 1) {
                        atomic_store_n(&mcb->is_full, 1, ATOMIC_RELEASE);
                    }
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

static void _free_large_object_real(void* block_addr, AllocBlock_t* header) {
    uint64_t size_class  = header->AllocSizeAligned;
    uint32_t obj_bit_loc = header->BitMapBitLocation.SCBBitLocation;
    uint32_t mcb_slot    = header->BitMapBitLocation.MCBBitLocation & ~IS_LARGE_PATH_FLAG;
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

    if (old_rem == MCB_SCB_COUNT - 1 && atomic_load_n(&l_scb->rem_count, ATOMIC_ACQUIRE) == MCB_SCB_COUNT) {
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
    uint32_t mcb_slot_raw = header->BitMapBitLocation.MCBBitLocation;
    int is_large_path = (mcb_slot_raw & IS_LARGE_PATH_FLAG) != 0;
    uint32_t mcb_slot = mcb_slot_raw & ~IS_LARGE_PATH_FLAG;

    uint64_t scb_addr    = header->BitMapBase;
    uint64_t mcb_addr    = header->MCBAddr;
    uint64_t block_addr  = header->BlockAddr;

    int32_t idx = GetSizeClassIndex(size_class);
    if (idx < 0 || idx >= SCT_MAX_CLASSES) return;

    uint32_t word_idx = obj_bit_loc / 64;
    uint32_t bit_idx  = obj_bit_loc % 64;
    MainControlBlock_t* mcb = (MainControlBlock_t*)mcb_addr;

    if (!is_large_path) {
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
        if (old_rem == target_rem - 1 && atomic_load_n(&scb->rem_count, ATOMIC_ACQUIRE) == target_rem) {
            if (__a_cas_p((volatile void*)&mcb->list_base[mcb_slot], scb, NULL) == scb) {
                push_deferred_scb(scb, 0);
            }
        }
    } else {
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
        uint64_t saved = header->AllocSizeAligned - size;
        if (saved > PAGE_SIZE) {
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