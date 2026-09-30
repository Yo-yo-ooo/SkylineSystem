// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <fs/fc.h>
#include <mem/heap.h>
#include <klib/algorithm/art.h>
#include <pdef.h>

extern "C" void *__memcpy(void *d, const void *s, uint64_t n);
extern void  spinlock_lock(spinlock_t* lock);
extern void  spinlock_unlock(spinlock_t* lock);

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif




static file_cache_cpu_t *g_fc_cpus[FC_MAX_CPUS];
static uint32_t g_num_active_cpus = 0;
static spinlock_t g_fc_init_lock = 0;

#pragma region Oscillate Node Pooling

typedef struct fc_oscillate_node {
    fc_oscillate_t data;
    struct fc_oscillate_node *next;
} fc_oscillate_node_t;

static fc_oscillate_node_t *g_osc_free_lists[FC_MAX_CPUS] = {0};
static spinlock_t g_osc_pool_locks[FC_MAX_CPUS] = {0};
static uint32_t g_osc_pool_sizes[FC_MAX_CPUS] = {0};

static fc_oscillate_t* fc_oscillate_alloc(file_cache_cpu_t *s) {
    spinlock_lock(&g_osc_pool_locks[s->cpu_id]);
    fc_oscillate_node_t *node = g_osc_free_lists[s->cpu_id];
    if (likely(node)) {
        if (likely(node->next)) PREFETCH_RH(node->next);   // 优化: 预取池中下一节点
        g_osc_free_lists[s->cpu_id] = node->next;
        g_osc_pool_sizes[s->cpu_id]--;
    }
    spinlock_unlock(&g_osc_pool_locks[s->cpu_id]);

    if (unlikely(!node)) {
        node = (fc_oscillate_node_t*)kmalloc(sizeof(fc_oscillate_node_t));
        if (unlikely(!node)) return NULL;
    }
    PREFETCH_W(&node->data);
    return &node->data;
}

static void fc_oscillate_free(file_cache_cpu_t *s, fc_oscillate_t *osc) {
    if (unlikely(!osc)) return;
    fc_oscillate_node_t *node = container_of(osc, fc_oscillate_node_t, data);

    spinlock_lock(&g_osc_pool_locks[s->cpu_id]);
    if (likely(g_osc_pool_sizes[s->cpu_id] < 1024)) {
        node->next = g_osc_free_lists[s->cpu_id];
        g_osc_free_lists[s->cpu_id] = node;
        g_osc_pool_sizes[s->cpu_id]++;
        spinlock_unlock(&g_osc_pool_locks[s->cpu_id]);
    } else {
        spinlock_unlock(&g_osc_pool_locks[s->cpu_id]);
        kfree(node);
    }
}

#pragma endregion

#pragma region Statistics & Heuristics

typedef struct {
    uint64_t count;
    uint64_t sum_freq;
    uint64_t sum_io;
} fc_stats_ctx_t;

static int fc_collect_stats_cb(void *data, const uint8_t *key, uint32_t key_len, void *value) {
    (void)key; (void)key_len;
    fc_stats_ctx_t *ctx = (fc_stats_ctx_t *)data;
    fc_oscillate_t *osc = (fc_oscillate_t *)value;
    ctx->count++;
    ctx->sum_freq += osc->freq;
    ctx->sum_io += osc->io_len;
    return 0;
}

// 轻量级 CRC32，仅校验首 256 字节以兼顾性能与安全性
static inline uint32_t fc_crc32_partial(const void *data, size_t len) {
    if (unlikely(!data || len == 0)) return 0;
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    size_t check_len = (len > 256) ? 256 : len;
    for (size_t i = 0; i < check_len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320 : (crc >> 1);
        }
    }
    return ~crc;
}

static void fc_update_averages_internal(file_cache_cpu_t *s) {
    s->avg_osc_cache = s->oscillate_tree.size > 0 ? s->total_oscillations / s->oscillate_tree.size : 0;
    s->smoothed_cache_bytes = (s->smoothed_cache_bytes * 7 + s->total_cache_bytes) / 8;

    uint32_t dyn_window = s->total_entries / 4;
    if (dyn_window < 16) dyn_window = 16;
    if (dyn_window > 256) dyn_window = 256;
    s->evict_scan_window = dyn_window;

    s->evict_hit_threshold = s->evict_scan_window / 4;
    if (s->evict_hit_threshold < 2) s->evict_hit_threshold = 2;
}

static bool file_cache_should_cache(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                                    uint64_t freq, uint64_t target_cache_len, uint64_t osc_count) {
    if (unlikely(s->io_congestion >= 90)) return false;

    // 极小文件概率准入
    if (target_cache_len > 0 && target_cache_len < FC_TINY_FILE_THRESHOLD) {
        if (osc_count == 0 && freq < 3) {
            uint32_t sample_mod = (freq <= 1) ? 8 : 4;
            if (s->soft_limit > 0) {
                uint64_t tiny_limit = (s->soft_limit / 100) * 15;
                if (tiny_limit > 0 && s->tiny_cache_bytes * 10 > tiny_limit * 8) {
                    sample_mod <<= 1; // 反压
                }
            }
            uint32_t pseudo_rand = (uint32_t)(s->clock ^ (key_len > 0 ? (uint64_t)key[0] : 0));
            if ((pseudo_rand & (sample_mod - 1)) != 0) return false;
        }
    }

    if (osc_count > 0 && s->avg_osc_cache > 0 && osc_count > s->avg_osc_cache) return true;

    if (s->soft_limit == 0 || s->total_cache_bytes < s->soft_limit) {
        if (target_cache_len < FC_TINY_FILE_THRESHOLD && s->soft_limit > 0) {
            uint64_t tiny_limit = (s->soft_limit / 100) * 15;
            if (tiny_limit > 0 && s->tiny_cache_bytes + target_cache_len > tiny_limit) return false;
        }
        return true;
    }

    if (freq >= s->avg_freq_cache || target_cache_len >= s->avg_io_cache) return true;
    return false;
}

static bool file_cache_should_evict(file_cache_cpu_t *s, file_cache_entry_t *cur) {
    // 预读冷数据直接淘汰
    if (cur->access_freq == 0) return true;

    if (cur->osc_count > 0 && s->avg_osc_cache > 0 && cur->osc_count > s->avg_osc_cache) return false;

    uint64_t dyn_residence = (s->clock - s->last_decay_tick) > 100 ? 100 : 10;
    if (s->clock - cur->create_tick < dyn_residence) return false;

    if (cur->access_freq >= s->avg_freq_cache || cur->total_io_len >= s->avg_io_cache) return false;

    return true;
}

#pragma endregion

#pragma region LRU & Memory Management

/* 修复(#GP): LRU 走链前校验指针——内核对象均为高半区(HHDM)地址,
   非规范/低半区指针必为已释放内存复用后的垃圾, 直接停止遍历 */
static inline bool fc_bad_ptr(const void *p) {
    return ((uint64_t)p >> 48) != 0xFFFF;
}

static inline void fc_lru_remove(file_cache_cpu_t *s, file_cache_entry_t *e) {
    /* 修复(#GP): 幂等化——条目已被摘链时(双删路径)直接返回,
       否则会经已释放条目的垃圾指针写坏 lru_head/lru_tail */
    if (unlikely(!e->lru_prev && !e->lru_next && s->lru_head != e && s->lru_tail != e))
        return;
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else             s->lru_head = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else             s->lru_tail = e->lru_prev;
    if (unlikely(s->decay_cursor == e)) s->decay_cursor = e->lru_next;
    e->lru_prev = e->lru_next = NULL;
}

static inline void fc_lru_push_back(file_cache_cpu_t *s, file_cache_entry_t *e) {
    e->lru_next = NULL;
    e->lru_prev = s->lru_tail;
    if (likely(s->lru_tail)) s->lru_tail->lru_next = e;
    else                     s->lru_head = e;
    s->lru_tail = e;
}

static inline void fc_lru_push_front(file_cache_cpu_t *s, file_cache_entry_t *e) {
    e->lru_prev = NULL;
    e->lru_next = s->lru_head;
    if (likely(s->lru_head)) s->lru_head->lru_prev = e;
    else                     s->lru_tail = e;
    s->lru_head = e;
}

static inline void fc_lru_move_to_back(file_cache_cpu_t *s, file_cache_entry_t *e) {
    if (unlikely(e->lru_next == NULL && s->lru_tail == e)) return;  // 优化: 已在尾部的快路径
    fc_lru_remove(s, e);
    fc_lru_push_back(s, e);
}

#ifdef __KERNEL_TEST_HOST__
/* 宿主测试: 把 SLUB 内部的 free 暴露给 ASAN —— 对象内存不会立刻回到 libc,
   ASAN 看不到 allocator 内部的释放; 毒化后任何复用/读取都会报出释放栈 */
extern "C" void __asan_poison_memory_region(const volatile void *addr, size_t size);
extern "C" void __asan_unpoison_memory_region(const volatile void *addr, size_t size);
/* 宿主 harness 提供的全局操作计数, 供守卫打印定位 */
extern uint64_t g_test_op;
extern "C" int art_verify(art_tree *t);
/* 故障注入: 强制 promote 的"插入后校验"失败一次 (回归测试用) */
static int g_fc_fail_once = 0;
extern "C" void fc_set_insert_fail_once(void) { g_fc_fail_once = 1; }

/* 双重释放/双重分配追踪(代际, 精确地址开地址散列) */
#define FC_TRACK_N 65536
static uintptr_t g_tk_p[FC_TRACK_N];      // 0 = 空槽
static uint32_t  g_tk_gen[FC_TRACK_N];
static uint32_t  g_tk_freed[FC_TRACK_N];
static uintptr_t g_tk_first_ra[FC_TRACK_N];
static uintptr_t g_tk_alloc_ra[FC_TRACK_N];
static inline uint32_t fc_slot(void *p) {
    uint64_t h = (uintptr_t)p;
    h ^= h >> 17; h *= 0x9E3779B97F4A7C15ULL; h ^= h >> 31;
    return (uint32_t)(h & (FC_TRACK_N - 1));
}
static inline void fc_track_alloc(void *p) {
    if (unlikely(!p)) return;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t s = (fc_slot(p) + i) & (FC_TRACK_N - 1);
        if (g_tk_p[s] == 0) {
            g_tk_p[s] = (uintptr_t)p; g_tk_gen[s] = 1; g_tk_freed[s] = 0;
            g_tk_alloc_ra[s] = (uintptr_t)__builtin_return_address(0);
            return;
        }
        if (g_tk_p[s] == (uintptr_t)p) {
            if (g_tk_freed[s] != g_tk_gen[s]) {
                printf_("[FCDBG] ALLOC OF LIVE ADDRESS op=%llu p=%p\n",
                        (unsigned long long)g_test_op, p);
            }
            g_tk_gen[s]++;
            g_tk_alloc_ra[s] = (uintptr_t)__builtin_return_address(0);
            return;
        }
    }
}
static inline void fc_track_free(file_cache_entry_t *e, uintptr_t ra) {
    if (unlikely(!e)) return;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t s = (fc_slot(e) + i) & (FC_TRACK_N - 1);
        if (g_tk_p[s] == (uintptr_t)e) {
            if (g_tk_gen[s] > 0 && g_tk_freed[s] == g_tk_gen[s]) {
                printf_("[FCDBG] TRUE DOUBLE FREE of entry %p, first_free_ra=0x%llx\n",
                        (void*)e, (unsigned long long)g_tk_first_ra[s]);
            }
            g_tk_freed[s] = g_tk_gen[s];
            g_tk_first_ra[s] = ra;
            return;
        }
    }
}
/* 插入 ART 前检查值是否为已释放条目 (释放后未经重新分配即被插入 = UAF) */
static inline void fc_track_insert(file_cache_entry_t *e, const char *site) {
    if (unlikely(!e)) return;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t s = (fc_slot(e) + i) & (FC_TRACK_N - 1);
        if (g_tk_p[s] == (uintptr_t)e) {
            if (g_tk_gen[s] > 0 && g_tk_freed[s] == g_tk_gen[s]) {
                printf_("[FCDBG] INSERT OF FREED ENTRY op=%llu site=%s e=%p\n",
                        (unsigned long long)g_test_op, site, (void*)e);
            }
            return;
        }
    }
}
/* 查询地址的追踪记录 (污染诊断用) */
extern "C" void fc_track_query(void *p) {
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t s = (fc_slot(p) + i) & (FC_TRACK_N - 1);
        if (g_tk_p[s] == (uintptr_t)p) {
            printf_("[FCTRK] p=%p gen=%u freed_gen=%u first_ra=0x%llx alloc_ra=0x%llx (allocated=%s, freed=%s)\n",
                    p, g_tk_gen[s], g_tk_freed[s], (unsigned long long)g_tk_first_ra[s],
                    (unsigned long long)g_tk_alloc_ra[s],
                    g_tk_gen[s] > 0 ? "yes" : "no",
                    (g_tk_freed[s] == g_tk_gen[s] && g_tk_gen[s] > 0) ? "yes" : "no");
            return;
        }
    }
    printf_("[FCTRK] p=%p 无记录\n", p);
}
#endif

static inline void fc_entry_free(file_cache_entry_t *e) {
    if (unlikely(!e)) return;
#ifdef __KERNEL_TEST_HOST__
    if (g_test_op == 565) printf_("[P565] fc_entry_free e=%p key=%p\n", (void*)e, (void*)e->key);
#endif
#ifdef __KERNEL_TEST_HOST__
    /* 释放时仍在其所属缓存的 ART 中 = 悬空指针 bug, 直接打印释放站点 */
    if (e->cpu_id < 64 && g_fc_cpus[e->cpu_id] &&
        art_search(&g_fc_cpus[e->cpu_id]->index, e->key, e->key_len) == e) {
        printf_("[FCDBG] FREE WHILE STILL IN ART: entry %p (key_len=%u) ra=0x%llx\n",
                (void*)e, e->key_len, (unsigned long long)__builtin_return_address(0));
    }
#endif
    if (e->data && e->data != e->inline_data) kfree(e->data);
    if (e->key) kfree(e->key);
#ifdef __KERNEL_TEST_HOST__
    fc_track_free(e, (uintptr_t)__builtin_return_address(0));
    __asan_poison_memory_region(e, sizeof(*e));
#endif
    kfree(e);
}

static void fc_update_oscillate(file_cache_cpu_t *s, file_cache_entry_t *v) {
    fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, v->key, v->key_len);
    if (unlikely(!osc)) {
        osc = fc_oscillate_alloc(s);
        if (likely(osc)) {
            osc->freq = v->access_freq;
            osc->io_len = v->total_io_len;
            osc->osc_count = v->osc_count + 1;
            osc->file_size = v->file_size;
            osc->file_id = v->file_id;
            art_insert(&s->oscillate_tree, v->key, v->key_len, (void *)osc);
            s->total_oscillations += osc->osc_count;
        }
    } else {
        osc->freq += v->access_freq;
        osc->io_len += v->total_io_len;
        s->total_oscillations -= osc->osc_count;
        osc->osc_count += (v->osc_count + 1);
        s->total_oscillations += osc->osc_count;
        if (v->file_size > 0) osc->file_size = v->file_size;
        osc->file_id = v->file_id;
    }
}

static file_cache_entry_t *fc_pick_and_unlink_victim(file_cache_cpu_t *s) {
    file_cache_entry_t *clean_fallback = NULL;
    int32_t scan_cnt = 0;
    bool hit = false;

    for (file_cache_entry_t *cur = s->lru_head; cur && (uint32_t)scan_cnt < s->evict_scan_window; cur = cur->lru_next) {
        if (unlikely(fc_bad_ptr(cur))) break; /* 修复(#GP): 链上垃圾指针, 停止 */
        if (likely(cur->lru_next)) PREFETCH_R(cur->lru_next);   // 优化: 预取 LRU 下一节点

        if (unlikely(cur->pending_reclaim && cur->pin_count == 0)) {
            /* 原实现不做 art_delete —— CRC 校验失败的条目仍挂在 ART 中,
               此处释放后 ART 残留悬垂指针。补上 art_delete (幂等, 不在树中返回 NULL) */
            void *av = art_delete(&s->index, cur->key, cur->key_len);
            if (unlikely(av == NULL)) { cur = cur->lru_next; continue; } /* 修复(#GP): 已摘除, 勿双删 */
            fc_lru_remove(s, cur);
            s->total_cache_bytes -= cur->data_len;
            if (cur->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= cur->data_len;
            s->total_cache_io    -= cur->total_io_len;
            s->total_cache_freq  -= cur->access_freq;
            s->total_entries--;
            s->evictions++;
            if (cur->access_freq == 0) s->readahead_evictions++;
            return cur;
        }

        if (unlikely(cur->state == FC_STATE_WRITEBACK_FAILED)) continue;

        if (cur->pin_count == 0 && cur->state == FC_STATE_CACHED && !cur->is_dirty) {
            bool is_protected = (cur->osc_count > 0 && s->avg_osc_cache > 0 && cur->osc_count > s->avg_osc_cache);
            bool is_young = (s->clock - cur->create_tick < 100);

            if (unlikely(!is_protected && !is_young && !clean_fallback)) clean_fallback = cur;

            if (file_cache_should_evict(s, cur)) {
                hit = true;
                s->evict_hit_count++;
                s->evict_miss_count = 0;
                if ((uint32_t)s->evict_hit_count >= s->evict_hit_threshold) s->evict_hit_count = 0; /* 修复: 符号比较 */

                void *art_val = art_delete(&s->index, cur->key, cur->key_len);
                if (likely(art_val)) {
                    fc_lru_remove(s, cur);
                    s->total_cache_bytes -= cur->data_len;
                    if (cur->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= cur->data_len;
                    s->total_cache_io    -= cur->total_io_len;
                    s->total_cache_freq  -= cur->access_freq;
                    s->total_entries--;
                    s->evictions++;
                    if (cur->access_freq == 0) s->readahead_evictions++;
                    return cur;
                } else {
                    cur->pending_reclaim = true;
                }
            }
            scan_cnt++;
        }
    }

    if (unlikely(!hit && clean_fallback)) {
        s->evict_hit_count = 0;
        s->evict_miss_count++;
        void *art_val = art_delete(&s->index, clean_fallback->key, clean_fallback->key_len);
        if (likely(art_val)) {
            fc_lru_remove(s, clean_fallback);
            s->total_cache_bytes -= clean_fallback->data_len;
            if (clean_fallback->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= clean_fallback->data_len;
            s->total_cache_io    -= clean_fallback->total_io_len;
            s->total_cache_freq  -= clean_fallback->access_freq;
            s->total_entries--;
            s->evictions++;
            if (clean_fallback->access_freq == 0) s->readahead_evictions++;
            return clean_fallback;
        }
    }
    return NULL;
}

// 容错分配：根据申请大小估算需要驱逐的页数
static void* fc_kmalloc_with_fallback(file_cache_cpu_t *s, size_t size) {
    void *ptr = kmalloc(size);
#ifdef __KERNEL_TEST_HOST__
    if (ptr) fc_track_alloc(ptr);
#endif
    if (unlikely(!ptr)) {
        spinlock_lock(&s->lock);
        uint32_t need_pages = (size + 4095) / 4096;
        if (need_pages == 0) need_pages = 1;
        for (uint32_t i = 0; i < need_pages; i++) {
            file_cache_entry_t *v = fc_pick_and_unlink_victim(s);
            if (likely(v)) {
                fc_update_oscillate(s, v);
                fc_entry_free(v);
            } else break;
        }
        spinlock_unlock(&s->lock);
        ptr = kmalloc(size);
    }
    return ptr;
}

static void* fc_kcalloc_with_fallback(file_cache_cpu_t *s, size_t n, size_t size) {
    void *ptr = kcalloc(n, size);
#ifdef __KERNEL_TEST_HOST__
    if (ptr) fc_track_alloc(ptr);
#endif
    if (unlikely(!ptr)) {
        spinlock_lock(&s->lock);
        size_t total_size = n * size;
        uint32_t need_pages = (total_size + 4095) / 4096;
        if (need_pages == 0) need_pages = 1;
        for (uint32_t i = 0; i < need_pages; i++) {
            file_cache_entry_t *v = fc_pick_and_unlink_victim(s);
            if (likely(v)) {
                fc_update_oscillate(s, v);
                fc_entry_free(v);
            } else break;
        }
        spinlock_unlock(&s->lock);
        ptr = kcalloc(n, size);
    }
    return ptr;
}

#pragma endregion

#pragma region Init & Destroy

void file_cache_cpu_init(file_cache_cpu_t *s, uint32_t cpu_id,
                        int32_t (*writeback_cb)(const uint8_t*, uint32_t, void*, size_t)) {
    if (unlikely(!s)) return;
    art_tree_init(&s->index);
    art_tree_init(&s->oscillate_tree);
    s->lock = 0;
    s->cpu_id = cpu_id;
    s->lru_head = s->lru_tail = NULL;
    s->decay_cursor = NULL;
    s->clock = 0;
    s->last_decay_tick = 0;

    s->total_cache_io = 0; s->total_cache_freq = 0;
    s->total_oscillations = 0;
    s->max_file_size = 0;
    s->total_cache_bytes = 0; s->dirty_cache_bytes = 0;
    s->smoothed_cache_bytes = 0;
    s->tiny_cache_bytes = 0;
    s->soft_limit = 0; s->hard_limit = 0;
    s->avg_io_cache = 4096; s->avg_freq_cache = 2; s->avg_osc_cache = 0;
    s->total_entries = 0;
    s->evict_scan_window = 16;
    s->evict_hit_count = 0; s->evict_miss_count = 0; s->evict_hit_threshold = 4;
    s->hits = 0; s->misses = 0; s->evictions = 0;
    s->migrations_in = 0; s->migrations_out = 0;
    s->readahead_evictions = 0;
    s->writeback_cb = writeback_cb;
    s->io_congestion = 0;
    s->total_writeback_failures = 0;

    spinlock_lock(&g_fc_init_lock);
    if (likely(cpu_id < FC_MAX_CPUS)) {
        g_fc_cpus[cpu_id] = s;
        if (cpu_id + 1 > g_num_active_cpus) g_num_active_cpus = cpu_id + 1;
    }
    spinlock_unlock(&g_fc_init_lock);
}

void file_cache_cpu_destroy(file_cache_cpu_t *s) {
    if (unlikely(!s)) return;
    spinlock_lock(&g_fc_init_lock);
    if (likely(s->cpu_id < FC_MAX_CPUS && g_fc_cpus[s->cpu_id] == s)) {
        g_fc_cpus[s->cpu_id] = NULL;
    }
    spinlock_unlock(&g_fc_init_lock);

    art_tree_destroy(&s->index);
    art_tree_destroy(&s->oscillate_tree);

    spinlock_lock(&g_osc_pool_locks[s->cpu_id]);
    fc_oscillate_node_t *node = g_osc_free_lists[s->cpu_id];
    while (node) {
        fc_oscillate_node_t *next = node->next;
        if (likely(next)) PREFETCH_R(next);
        kfree(node);
        node = next;
    }
    g_osc_free_lists[s->cpu_id] = NULL;
    g_osc_pool_sizes[s->cpu_id] = 0;
    spinlock_unlock(&g_osc_pool_locks[s->cpu_id]);
}

void file_cache_set_limits(file_cache_cpu_t *s, uint64_t soft_limit, uint64_t hard_limit) {
    if (unlikely(!s)) return;
    spinlock_lock(&s->lock);
    s->soft_limit = soft_limit;
    s->hard_limit = hard_limit;
    spinlock_unlock(&s->lock);
}

#pragma endregion

#pragma region Invalidation & Migration

static void fc_broadcast_invalidate(file_cache_cpu_t *src_s, const uint8_t *key, uint32_t key_len) {
    const uint32_t ncpu = g_num_active_cpus;   // 咨询式读取
    for (uint32_t i = 0; i < ncpu; i++) {
        if (likely(i + 1 < ncpu)) PREFETCH_R(&g_fc_cpus[i + 1]);   // 优化
        if (unlikely(i == src_s->cpu_id)) continue;
        file_cache_cpu_t *s = g_fc_cpus[i];
        if (unlikely(!s)) continue;

        file_cache_entry_t *entry_to_free = NULL;
        fc_oscillate_t *osc_to_free = NULL;

        spinlock_lock(&s->lock);
        file_cache_entry_t *e = (file_cache_entry_t *)art_search(&s->index, key, key_len);
        if (e) {
            if (e->pin_count > 0) {
                e->state = FC_STATE_INVALID;
            } else {
                void *art_val = art_delete(&s->index, e->key, e->key_len);
                if (likely(art_val)) {
                    fc_lru_remove(s, e);
                    s->total_cache_bytes -= e->data_len;
                    if (e->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= e->data_len;
                    if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
                    s->total_cache_io    -= e->total_io_len;
                    s->total_cache_freq  -= e->access_freq;
                    s->total_entries--;
                    entry_to_free = e;
                } else {
                    e->pending_reclaim = true;
                }
            }
        }
        osc_to_free = (fc_oscillate_t *)art_delete(&s->oscillate_tree, key, key_len);
        if (osc_to_free) s->total_oscillations -= osc_to_free->osc_count;
        spinlock_unlock(&s->lock);

        if (unlikely(entry_to_free)) fc_entry_free(entry_to_free);
        if (unlikely(osc_to_free)) fc_oscillate_free(s, osc_to_free);
    }
}

void file_cache_check_load(file_cache_cpu_t *src, uint32_t load_factor) {
    if (unlikely(!src || load_factor < 80)) return;

    uint32_t best_dst = (uint32_t)-1;
    uint64_t lowest_load = 100;

    // 以下为无锁咨询式读取 (与 idle_handler 的周期性调用配合, 过时数据无害)
    uint64_t src_load = (src->soft_limit > 0) ? (src->smoothed_cache_bytes * 100 / src->soft_limit) : 0;
    if (src_load < 80) return;

    const uint32_t ncpu = g_num_active_cpus;
    for (uint32_t i = 0; i < ncpu; i++) {
        if (likely(i + 1 < ncpu)) PREFETCH_R(&g_fc_cpus[i + 1]);   // 优化
        if (unlikely(i == src->cpu_id || !g_fc_cpus[i])) continue;
        file_cache_cpu_t *dst = g_fc_cpus[i];

        uint64_t dst_load = (dst->soft_limit > 0) ? (dst->smoothed_cache_bytes * 100 / dst->soft_limit) : 0;
        if (dst_load < lowest_load) {
            lowest_load = dst_load;
            best_dst = i;
        }
    }
    if (unlikely(best_dst == (uint32_t)-1)) return;

    file_cache_cpu_t *dst = g_fc_cpus[best_dst];

    if (src->cpu_id < best_dst) {
        spinlock_lock(&src->lock);
        spinlock_lock(&dst->lock);
    } else {
        spinlock_lock(&dst->lock);
        spinlock_lock(&src->lock);
    }

    uint32_t dyn_migrate_batch = src->total_entries / 16;
    if (dyn_migrate_batch < 8) dyn_migrate_batch = 8;
    if (dyn_migrate_batch > 64) dyn_migrate_batch = 64;

    file_cache_entry_t **victims = (file_cache_entry_t**)kmalloc(sizeof(file_cache_entry_t*) * dyn_migrate_batch);
    if (unlikely(!victims)) {
        spinlock_unlock(&src->lock);
        spinlock_unlock(&dst->lock);
        return;
    }
    int vic_cnt = 0;

    int32_t migrated = 0, scanned = 0;
    file_cache_entry_t *cur = src->lru_head;
    while (cur && (uint32_t)migrated < dyn_migrate_batch && (uint32_t)scanned < src->total_entries) {
        file_cache_entry_t *next = cur->lru_next;
        if (likely(next)) PREFETCH_R(next);   // 优化: 预取 LRU 下一节点
        scanned++;
        if (cur->pin_count > 0 || cur->state != FC_STATE_CACHED || cur->is_dirty) {
            cur = next; continue;
        }

        if (dst->soft_limit > 0 && dst->smoothed_cache_bytes + cur->data_len > dst->soft_limit) break;

        fc_oscillate_t *osc = (fc_oscillate_t *)art_delete(&src->oscillate_tree, cur->key, cur->key_len);
        if (osc) src->total_oscillations -= osc->osc_count;

        if (art_search(&dst->index, cur->key, cur->key_len) != NULL) {
            void *art_val = art_delete(&src->index, cur->key, cur->key_len);
            if (likely(art_val)) {
                fc_lru_remove(src, cur);
                src->total_cache_bytes -= cur->data_len;
                if (cur->data_len < FC_TINY_FILE_THRESHOLD) src->tiny_cache_bytes -= cur->data_len;
                src->total_cache_io    -= cur->total_io_len;
                src->total_cache_freq  -= cur->access_freq;
                src->total_entries--;
                src->migrations_out++;
                victims[vic_cnt++] = cur;
            } else cur->pending_reclaim = true;

            if (osc) {
                fc_oscillate_t *dst_osc = (fc_oscillate_t *)art_search(&dst->oscillate_tree, cur->key, cur->key_len);
                if (dst_osc) {
                    dst_osc->freq += osc->freq;
                    dst_osc->io_len += osc->io_len;
                    dst_osc->osc_count += osc->osc_count;
                    dst->total_oscillations += osc->osc_count;
                    fc_oscillate_free(src, osc);
                } else {
                    art_insert(&dst->oscillate_tree, cur->key, cur->key_len, (void *)osc);
                    dst->total_oscillations += osc->osc_count;
                }
            }
            cur = next; continue;
        }

        void *art_val = art_delete(&src->index, cur->key, cur->key_len);
        if (unlikely(!art_val)) {
            cur->pending_reclaim = true;
            if (osc) {
                art_insert(&src->oscillate_tree, cur->key, cur->key_len, (void *)osc);
                src->total_oscillations += osc->osc_count;
            }
            cur = next; continue;
        }
        fc_lru_remove(src, cur);
        src->total_cache_bytes -= cur->data_len;
        if (cur->data_len < FC_TINY_FILE_THRESHOLD) src->tiny_cache_bytes -= cur->data_len;
        src->total_cache_io    -= cur->total_io_len;
        src->total_cache_freq  -= cur->access_freq;
        src->total_entries--;
        src->migrations_out++;

        cur->cpu_id = best_dst;
        art_insert(&dst->index, cur->key, cur->key_len, (void *)cur);
#ifdef __KERNEL_TEST_HOST__
        fc_track_insert(cur, "check_load");
#endif
        fc_lru_push_back(dst, cur);

        dst->total_entries++;
        dst->total_cache_bytes += cur->data_len;
        if (cur->data_len < FC_TINY_FILE_THRESHOLD) dst->tiny_cache_bytes += cur->data_len;
        dst->total_cache_io += cur->total_io_len;
        dst->total_cache_freq += cur->access_freq;
        if (cur->file_size > dst->max_file_size) dst->max_file_size = cur->file_size;
        dst->migrations_in++;

        if (osc) {
            art_insert(&dst->oscillate_tree, cur->key, cur->key_len, (void *)osc);
            dst->total_oscillations += osc->osc_count;
        }

        migrated++;
        cur = next;
    }

    spinlock_unlock(&src->lock);
    spinlock_unlock(&dst->lock);

    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
    kfree(victims);
}

#pragma endregion

#pragma region Get / Put / Record / Promote / Readahead

void *file_cache_get(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                     size_t io_len, size_t *out_len, file_cache_entry_t **out_entry) {
    if (unlikely(!s || !key || key_len == 0)) return NULL;

    spinlock_lock(&s->lock);
    file_cache_entry_t *e = (file_cache_entry_t *)art_search(&s->index, key, key_len);

    if (likely(e)) {
        /* 修复: 被跨 CPU 广播置为 INVALID/FLUSHING/FAILED 的条目不得继续
           命中, 否则 fwrite 之后其他核会读回陈旧数据 */
        if (unlikely(e->state != FC_STATE_CACHED)) {
            spinlock_unlock(&s->lock);
            return NULL;
        }
        // 数据完整性校验 (仅堆数据; 内联数据随条目生存, 由条目自身完整性覆盖)
        if (e->data && e->data != e->inline_data) {
            if (unlikely(fc_crc32_partial(e->data, e->data_len) != e->crc32)) {
                s->misses++;
                e->state = FC_STATE_INVALID;
                e->pending_reclaim = true;
                spinlock_unlock(&s->lock);
                return NULL;
            }
        }

        s->hits++;
        e->access_freq = (e->access_freq == 0) ? 1 : e->access_freq + 1;
        e->total_io_len += io_len;
        e->last_access_tick = s->clock;
        s->total_cache_io += io_len;
        s->total_cache_freq++;
        fc_lru_move_to_back(s, e);
        e->pin_count++;
        if (out_len) *out_len = e->data_len;
        if (out_entry) *out_entry = e;
        void *data = e->data;
        if (unlikely(e->data && e->data != e->inline_data)) PREFETCH_RH(e->data);   // 优化
        spinlock_unlock(&s->lock);
        return data;
    }
    s->misses++;
    spinlock_unlock(&s->lock);

    /*  跨核迁移查找改为全环回绕扫描 (原实现只向后扫, 低编号 CPU 的条目永远查不到) */
    const uint32_t ncpu = g_num_active_cpus;
    for (uint32_t k = 1; k < ncpu; k++) {
        uint32_t i = (s->cpu_id + k) % ncpu;
        file_cache_cpu_t *rs = g_fc_cpus[i];
        if (unlikely(!rs)) continue;

        spinlock_lock(&rs->lock);
        file_cache_entry_t *re = (file_cache_entry_t *)art_search(&rs->index, key, key_len);
#ifdef __KERNEL_TEST_HOST__
        if (unlikely(re && (re->key_len > 4096 || re->cpu_id >= 64 || re->pin_count > 1000000))) {
            printf_("[FCDBG] GET-MIGRATE GARBAGE ENTRY op=%llu rs=%u key=%.*s re=%p key_len=%u cpu_id=%u\n",
                    (unsigned long long)g_test_op, rs->cpu_id, (int)key_len, key, (void*)re,
                    re->key_len, re->cpu_id);
        }
#endif
        if (re && !re->is_dirty && re->pin_count == 0 && re->state == FC_STATE_CACHED) {
            if (s->soft_limit > 0 && s->smoothed_cache_bytes + re->data_len > s->soft_limit) {
                spinlock_unlock(&rs->lock); continue;
            }

            void *art_val = art_delete(&rs->index, re->key, re->key_len);
            if (likely(art_val)) {
                fc_lru_remove(rs, re);
                rs->total_cache_bytes -= re->data_len;
                if (re->data_len < FC_TINY_FILE_THRESHOLD) rs->tiny_cache_bytes -= re->data_len;
                rs->total_cache_io    -= re->total_io_len;
                rs->total_cache_freq  -= re->access_freq;
                rs->total_entries--;
                rs->migrations_out++;
                spinlock_unlock(&rs->lock);

                spinlock_lock(&s->lock);
                re->cpu_id = s->cpu_id;
                art_insert(&s->index, re->key, re->key_len, (void *)re);
#ifdef __KERNEL_TEST_HOST__
                fc_track_insert(re, "get-migrate");
#endif
                fc_lru_push_back(s, re);

                s->hits++;   // 优化: 迁移命中也计入命中率统计
                s->total_entries++;
                s->total_cache_bytes += re->data_len;
                if (re->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes += re->data_len;
                s->total_cache_io += re->total_io_len;
                s->total_cache_freq += re->access_freq;
                if (re->file_size > s->max_file_size) s->max_file_size = re->file_size;
                s->migrations_in++;

                re->pin_count++;
                if (out_len) *out_len = re->data_len;
                if (out_entry) *out_entry = re;
                void *data = re->data;
                spinlock_unlock(&s->lock);
                return data;
            }
        }
        spinlock_unlock(&rs->lock);
    }

    return NULL;
}

void file_cache_put(file_cache_cpu_t *s, file_cache_entry_t *e) {
    if (unlikely(!s || !e)) return;

    spinlock_lock(&s->lock);
    bool need_free = false;
    if (likely(e->pin_count > 0)) e->pin_count--;

    // 脏页重新计算 CRC
    if (e->is_dirty && e->data && e->data != e->inline_data) {
        e->crc32 = fc_crc32_partial(e->data, e->data_len);
    }

    if (e->pin_count == 0 && (e->state == FC_STATE_INVALID || e->pending_reclaim)) {
        if (!e->pending_reclaim) {
            void *art_val = art_delete(&s->index, e->key, e->key_len);
            if (unlikely(!art_val)) {
                e->pending_reclaim = true;
            } else {
                fc_lru_remove(s, e);
                s->total_cache_bytes -= e->data_len;
                if (e->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= e->data_len;
                if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
                s->total_cache_io    -= e->total_io_len;
                s->total_cache_freq  -= e->access_freq;
                s->total_entries--;
                need_free = true;
            }
        } else {
            /* 防御性 art_delete —— pending_reclaim 条目可能仍在 ART 中
               (如 CRC 失败路径设置标志后无人 put), 不删除会留下悬垂指针 */
            void *av = art_delete(&s->index, e->key, e->key_len);
            if (unlikely(av == NULL)) {
                /* 修复(#GP): 已不在 ART = 已被 pick_and_unlink_victim 等路径
                   摘除并释放, 不能再 lru_remove/free, 否则写坏 LRU(双删) */
                spinlock_unlock(&s->lock);
                return;
            }
            fc_lru_remove(s, e);
            s->total_cache_bytes -= e->data_len;
            if (e->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= e->data_len;
            if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
            s->total_cache_io    -= e->total_io_len;
            s->total_cache_freq  -= e->access_freq;
            s->total_entries--;
            need_free = true;
        }
    }
    spinlock_unlock(&s->lock);
    if (unlikely(need_free)) fc_entry_free(e);
}

int32_t file_cache_record_io(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                             size_t io_len, void *data_if_promote, uint64_t file_size, uint64_t file_id) {
    if (unlikely(!s || !key || key_len == 0 || io_len == 0)) return -1;

    
    bool should = false;

    spinlock_lock(&s->lock);

    if (file_size > 0 && file_size > s->max_file_size) s->max_file_size = file_size;

    file_cache_entry_t *e = (file_cache_entry_t *)art_search(&s->index, key, key_len);

    if (likely(e)) {
        e->access_freq = (e->access_freq == 0) ? 1 : e->access_freq + 1;
        e->total_io_len += io_len;
        if (file_size > 0) e->file_size = file_size;
        if (file_id != 0) e->file_id = file_id;
        s->total_cache_io += io_len;
        s->total_cache_freq++;
        fc_lru_move_to_back(s, e);
        spinlock_unlock(&s->lock);

        if (unlikely(data_if_promote)) kfree(data_if_promote);
        return 0;
    }

    {
        fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, key, key_len);
        uint64_t cur_freq  = osc ? osc->freq + 1 : 1;
        uint64_t cur_osc   = osc ? osc->osc_count : 0;
        uint64_t cur_fsize = osc ? osc->file_size : file_size;

        if (cur_fsize > 0 && cur_fsize > s->max_file_size) s->max_file_size = cur_fsize;

        should = file_cache_should_cache(s, key, key_len, cur_freq, io_len, cur_osc);   
    }
    spinlock_unlock(&s->lock);

    if (should && data_if_promote) {
        int32_t r = file_cache_promote(s, key, key_len, data_if_promote, io_len, false, file_size, file_id);
        if (r != 0) {
            spinlock_lock(&s->lock);
            fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, key, key_len);   
            if (!osc) {
                osc = fc_oscillate_alloc(s);
                if (osc) {
                    osc->freq = 1; osc->io_len = io_len; osc->osc_count = 0;
                    osc->file_size = file_size; osc->file_id = file_id;
                    art_insert(&s->oscillate_tree, key, key_len, (void *)osc);
                }
            } else {
                osc->freq++; osc->io_len += io_len;
                if (file_size > 0) osc->file_size = file_size;
            }
            spinlock_unlock(&s->lock);
            kfree(data_if_promote);
            return r;
        }
        return (int32_t)io_len;
    } else {
        spinlock_lock(&s->lock);
        fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, key, key_len);  
        if (!osc) {
            osc = fc_oscillate_alloc(s);
            if (osc) {
                osc->freq = 1; osc->io_len = io_len; osc->osc_count = 0;
                osc->file_size = file_size; osc->file_id = file_id;
                art_insert(&s->oscillate_tree, key, key_len, (void *)osc);
            }
        } else {
            osc->freq++; osc->io_len += io_len;
        }
        spinlock_unlock(&s->lock);
        if (!should && data_if_promote) kfree(data_if_promote);
        return 0;
    }
}

static int fc_try_evict_for_space(file_cache_cpu_t *s, uint64_t need_space, file_cache_entry_t **victims, int max_victims) {
    int vic_cnt = 0;
    while (s->hard_limit > 0 && s->total_cache_bytes + need_space > s->hard_limit && vic_cnt < max_victims) {
        file_cache_entry_t *v = fc_pick_and_unlink_victim(s);
        if (likely(v)) {
            fc_update_oscillate(s, v);
            victims[vic_cnt++] = v;
        } else break;
    }
    return vic_cnt;
}

int32_t file_cache_promote(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                           void *data, size_t data_len, bool is_dirty, uint64_t file_size, uint64_t file_id) {
    if (unlikely(!s || !key || key_len == 0 || !data || data_len == 0)) return -1;

    size_t alloc_size = sizeof(file_cache_entry_t);
    bool use_inline = (data_len <= FC_INLINE_DATA_SIZE);
    if (use_inline) alloc_size += data_len;

    file_cache_entry_t *e = (file_cache_entry_t *)fc_kcalloc_with_fallback(s, 1, alloc_size);
    if (unlikely(!e)) return FC_ERR_NO_MEMORY;

    e->key = (uint8_t *)fc_kmalloc_with_fallback(s, key_len);
    if (unlikely(!e->key)) { kfree(e); return FC_ERR_NO_MEMORY; }
#ifdef __KERNEL_TEST_HOST__
    if (g_test_op == 565) printf_("[P565] promote s=%u key='%.*s' len=%u data_len=%zu e=%p e->key=%p\n",
        s->cpu_id, (int)key_len, key, key_len, data_len, (void*)e, (void*)e->key);
    if (g_test_op == 565) { printf_("[P565] pre-insert verify:\n"); art_verify(&s->index); }
#endif

    __memcpy(e->key, key, key_len);
    e->key_len = key_len;
    e->cpu_id = s->cpu_id;
    e->is_dirty = is_dirty;
    e->file_size = file_size;
    e->file_id = file_id;
    e->data_len = data_len;
    e->state = FC_STATE_CACHED;
    e->create_tick = s->clock;
    e->last_access_tick = s->clock;
    e->total_io_len = data_len;
    e->access_freq = 1;

    bool need_broadcast = false;
    file_cache_entry_t *victims[8] = {0};
    int vic_cnt = 0;
    void *old_data_to_free = NULL;

    spinlock_lock(&s->lock);

    if (file_size > 0 && file_size > s->max_file_size) s->max_file_size = file_size;

    if (unlikely(is_dirty && s->io_congestion >= 90)) {
        spinlock_unlock(&s->lock);
        kfree(e->key); kfree(e);
        return FC_ERR_NO_MEMORY;
    }

    file_cache_entry_t *exist = (file_cache_entry_t *)art_search(&s->index, key, key_len);
    if (exist) {
        if (unlikely(exist->pin_count > 0)) {
            spinlock_unlock(&s->lock);
            kfree(e->key); kfree(e);
            return (exist->state == FC_STATE_FLUSHING) ? FC_ERR_FLUSHING : FC_ERR_NO_MEMORY;
        }

        old_data_to_free = (exist->data != exist->inline_data) ? exist->data : NULL;
        s->total_cache_bytes -= exist->data_len;
        if (exist->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= exist->data_len;
        if (exist->is_dirty) s->dirty_cache_bytes -= exist->data_len;
        s->total_cache_io    -= exist->total_io_len;

        exist->data_len = data_len;
        exist->total_io_len += data_len;
        exist->is_dirty = is_dirty;
        exist->writeback_retries = 0;
        /* 修复(#GP): 复用条目必须清掉 pending/INVALID 状态, 否则后续 put
           会按"待回收"路径再次摘链释放(双删) */
        exist->state = FC_STATE_CACHED;
        exist->pending_reclaim = false;
        exist->access_freq = (exist->access_freq == 0) ? 1 : exist->access_freq + 1;
        if (file_size > 0) exist->file_size = file_size;
        if (file_id != 0) exist->file_id = file_id;


        if (use_inline) {
            /* 修复(#GP/内存越界): 旧条目按当初的 data_len 分配内联容量,
               复用前必须校验新 data_len 是否仍放得下; 放不下则摘除旧条目
               (走下方新条目路径, e 已在函数开头分配好)。
               注意: 1030-1033 已扣减旧条目的字节统计, 此处只摘链与计数 */
            size_t cap = SLUB::TryGetSize(exist);
            if (cap == 0) cap = sizeof(file_cache_entry_t);
            if (unlikely(data_len > cap - sizeof(file_cache_entry_t))) {
                void *av = art_delete(&s->index, exist->key, exist->key_len);
                if (av) {
                    fc_lru_remove(s, exist);
                    s->total_cache_freq -= exist->access_freq;
                    s->total_entries--;
                }
                fc_entry_free(exist);   /* 含 old_data_to_free, 锁内 kfree 安全 */
                goto insert_new;        /* 锁保持持有, 与下方新条目路径一致 */
            }
            __memcpy(exist->inline_data, data, data_len);
            exist->data = exist->inline_data;
            /* 内联数据不参与 CRC 校验 (get 跳过), 无需设置 */
        } else {
            exist->data = data;
            exist->crc32 = fc_crc32_partial(data, data_len);   
        }

        s->total_cache_bytes += data_len;
        if (data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes += data_len;
        if (is_dirty) s->dirty_cache_bytes += data_len;
        s->total_cache_io += exist->total_io_len;
        fc_lru_move_to_back(s, exist);

        if (is_dirty) need_broadcast = true;
        spinlock_unlock(&s->lock);

        if (unlikely(old_data_to_free)) kfree(old_data_to_free);
        if (use_inline) kfree(data);  
        kfree(e->key); kfree(e);

        if (unlikely(need_broadcast)) fc_broadcast_invalidate(s, key, key_len);
        return 0;
    }

insert_new:;
    if (unlikely(is_dirty && s->total_cache_bytes > 0)) {
        uint32_t dyn_dirty_limit = 80 - (s->io_congestion / 2);
        if (dyn_dirty_limit < 20) dyn_dirty_limit = 20;
        if ((s->dirty_cache_bytes * 100 / s->total_cache_bytes) > dyn_dirty_limit) {
            spinlock_unlock(&s->lock);
            kfree(e->key); kfree(e);
            return FC_ERR_NO_MEMORY;
        }
    }

    if (unlikely(s->hard_limit > 0 && s->total_cache_bytes + data_len > s->hard_limit)) {
        vic_cnt = fc_try_evict_for_space(s, data_len, victims, 8);
        if (s->total_cache_bytes + data_len > s->hard_limit) {
            spinlock_unlock(&s->lock);
            kfree(e->key); kfree(e);
            for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
            return FC_ERR_NO_MEMORY;
        }
    }

    fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, key, key_len);
    if (osc) {
        e->access_freq = osc->freq;
        e->osc_count = osc->osc_count;
    }

    art_insert(&s->index, e->key, e->key_len, (void *)e);
#ifdef __KERNEL_TEST_HOST__
    fc_track_insert(e, "promote");
    if (g_test_op == 565) printf_("[P565] after insert: e=%p e->key=%p\n", (void*)e, (void*)e->key);
#endif
    if (unlikely(art_search(&s->index, e->key, e->key_len) != e
#ifdef __KERNEL_TEST_HOST__
        || __atomic_exchange_n(&g_fc_fail_once, 0, __ATOMIC_RELAXED)
#endif
        )) {
        /* 修复(#GP/UAF): 插入后校验失败时的处理必须保证不留悬垂值 ——
           1) 幂等删除: 若树中残留本键的值, 先摘除; 是他人条目则恢复;
           2) 重试一次插入 (偶发失败不应立即放弃);
           3) 仍失败才释放, 且此时树中已保证无本键残留 */
        void *stale = art_delete(&s->index, e->key, e->key_len);
        if (unlikely(stale && stale != e)) {
            art_insert(&s->index, e->key, e->key_len, stale);
            spinlock_unlock(&s->lock);
            kfree(e->key); kfree(e);
            for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
            return -4;
        }
        art_insert(&s->index, e->key, e->key_len, (void *)e);
        if (unlikely(art_search(&s->index, e->key, e->key_len) != e)) {
            art_delete(&s->index, e->key, e->key_len);
            spinlock_unlock(&s->lock);
            kfree(e->key); kfree(e);
            for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
            return -4;
        }
    }

    fc_lru_push_back(s, e);
    s->total_entries++;
    s->total_cache_bytes += data_len;
    if (data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes += data_len;
    if (is_dirty) s->dirty_cache_bytes += data_len;
    s->total_cache_io    += e->total_io_len;
    s->total_cache_freq  += e->access_freq;

    if (use_inline) {
        __memcpy(e->inline_data, data, data_len);
        e->data = e->inline_data;
        kfree(data);
    } else {
        e->data = data;
        e->crc32 = fc_crc32_partial(data, data_len);
    }

    if (is_dirty) need_broadcast = true;
    spinlock_unlock(&s->lock);

    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
    if (unlikely(need_broadcast)) fc_broadcast_invalidate(s, key, key_len);

    return 0;
}

int32_t file_cache_readahead(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                             void *data, size_t data_len, uint64_t file_size, uint64_t file_id) {
    if (unlikely(!s || !key || key_len == 0 || !data || data_len == 0)) {
        if (data) kfree(data);
        return -1;
    }

    spinlock_lock(&s->lock);
    // 内存占用过高或已存在缓存，直接丢弃预读数据
    if (unlikely((s->soft_limit > 0 && s->total_cache_bytes > (s->soft_limit * 80) / 100) ||
        (art_search(&s->index, key, key_len) != NULL))) {
        spinlock_unlock(&s->lock);
        kfree(data);
        return 0;
    }

    if (s->hard_limit > 0 && s->total_cache_bytes + data_len > s->hard_limit) {
        file_cache_entry_t *victims[8] = {0};
        int vic_cnt = fc_try_evict_for_space(s, data_len, victims, 8);
        if (s->total_cache_bytes + data_len > s->hard_limit) {
            spinlock_unlock(&s->lock);
            for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
            kfree(data);
            return 0;
        }
        for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
    }
    spinlock_unlock(&s->lock);

    size_t alloc_size = sizeof(file_cache_entry_t);
    bool use_inline = (data_len <= FC_INLINE_DATA_SIZE);
    if (use_inline) alloc_size += data_len;

    file_cache_entry_t *e = (file_cache_entry_t *)fc_kcalloc_with_fallback(s, 1, alloc_size);
    if (unlikely(!e)) { kfree(data); return FC_ERR_NO_MEMORY; }

    e->key = (uint8_t *)fc_kmalloc_with_fallback(s, key_len);
    if (unlikely(!e->key)) { kfree(e); kfree(data); return FC_ERR_NO_MEMORY; }
    __memcpy(e->key, key, key_len);

    e->key_len = key_len;
    e->cpu_id = s->cpu_id;
    e->file_size = file_size;
    e->file_id = file_id;
    e->data_len = data_len;
    e->state = FC_STATE_CACHED;
    e->create_tick = s->clock;
    e->total_io_len = 0;
    e->access_freq = 0; // 标记为预读冷数据

    if (use_inline) {
        __memcpy(e->inline_data, data, data_len);
        e->data = e->inline_data;
        kfree(data);
    } else {
        e->data = data;
        e->crc32 = fc_crc32_partial(data, data_len);
    }

    spinlock_lock(&s->lock);
    art_insert(&s->index, e->key, e->key_len, (void *)e);
#ifdef __KERNEL_TEST_HOST__
    fc_track_insert(e, "readahead");
#endif
    /* 优化: 与 promote 同款的并发插入自检 —— 构造期间(未持锁)他人可能已插入同 key,
       避免 LRU 中残留不可达的重复条目 */
    if (unlikely(art_search(&s->index, e->key, e->key_len) != e)) {
        spinlock_unlock(&s->lock);
        fc_entry_free(e);
        return 0;
    }
    fc_lru_push_front(s, e); // 挂入头部

    s->total_entries++;
    s->total_cache_bytes += data_len;
    if (data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes += data_len;

    spinlock_unlock(&s->lock);
    return 0;
}

int32_t file_cache_invalidate(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len) {
    if (unlikely(!s || !key || key_len == 0)) return -1;
    fc_broadcast_invalidate(s, key, key_len);
    return 0;
}

#pragma endregion

#pragma region Fsync & Background Maintenance

int32_t file_cache_fsync(file_cache_cpu_t *s, uint64_t file_id) {
    if (unlikely(!s)) return -1;

    file_cache_entry_t **flush_list = (file_cache_entry_t**)kmalloc(sizeof(file_cache_entry_t*) * FC_FSYNC_BATCH_SIZE);
    if (unlikely(!flush_list)) return -1;

    int32_t final_rc = 0;
    const uint32_t ncpu = g_num_active_cpus;

    for (uint32_t i = 0; i < ncpu; i++) {
        file_cache_cpu_t *target_s = g_fc_cpus[i];
        if (unlikely(!target_s)) continue;

        while (true) {
            uint32_t flush_cnt = 0;

            spinlock_lock(&target_s->lock);
            file_cache_entry_t *cur = target_s->lru_head;
            while (cur && flush_cnt < FC_FSYNC_BATCH_SIZE) {
                file_cache_entry_t *next = cur->lru_next;
                if (likely(next)) PREFETCH_R(next);   // 优化
                if (cur->file_id == file_id && cur->is_dirty &&
                    cur->state == FC_STATE_CACHED && cur->pin_count == 0) {
                    cur->state = FC_STATE_FLUSHING;
                    cur->pin_count++;
                    flush_list[flush_cnt++] = cur;
                }
                cur = next;
            }
            spinlock_unlock(&target_s->lock);

            if (flush_cnt == 0) break;

            if (likely(target_s->writeback_cb)) {
                for (uint32_t j = 0; j < flush_cnt; j++) {
                    file_cache_entry_t *e = flush_list[j];
                    int32_t rc = target_s->writeback_cb(e->key, e->key_len, e->data, e->data_len);
                    if (likely(rc == 0)) {
                        e->writeback_retries = 0;
                        if (target_s->io_congestion > 0) target_s->io_congestion--;
                    } else {
                        e->writeback_retries++;
                        target_s->io_congestion += 10;
                        if (target_s->io_congestion > 100) target_s->io_congestion = 100;
                        target_s->total_writeback_failures++;
                        final_rc = -1;
                    }
                }
            }

            spinlock_lock(&target_s->lock);
            for (uint32_t j = 0; j < flush_cnt; j++) {
                file_cache_entry_t *e = flush_list[j];
                e->state = FC_STATE_CACHED;
                e->pin_count--;

                if (e->writeback_retries >= 5) e->state = FC_STATE_WRITEBACK_FAILED;

                if (e->writeback_retries == 0 && e->is_dirty) {
                    e->is_dirty = false;
                    target_s->dirty_cache_bytes -= e->data_len;
                }
            }
            spinlock_unlock(&target_s->lock);
        }
    }

    kfree(flush_list);
    return final_rc;
}

typedef struct {
    uint64_t total_cached;
    uint64_t quota;
} fc_file_stat_t;

static int fc_free_file_stat_cb(void *data, const uint8_t *key, uint32_t key_len, void *value) {
    (void)data; (void)key; (void)key_len;
    if (value) kfree(value);
    return 0;
}

void file_cache_idle_handler(file_cache_cpu_t *s) {
    if (unlikely(!s)) return;

    // phase 0: 离群统计与拥塞控制
    spinlock_lock(&s->lock);
    fc_stats_ctx_t stats_ctx = {0, 0, 0};
    art_iter(&s->oscillate_tree, fc_collect_stats_cb, &stats_ctx);
    if (stats_ctx.count > 0) {
        uint64_t mean_freq = stats_ctx.sum_freq / stats_ctx.count;
        uint64_t mean_io = stats_ctx.sum_io / stats_ctx.count;
        s->avg_freq_cache = mean_freq + (mean_freq >> 2);
        s->avg_io_cache = mean_io + (mean_io >> 2);
    } else {
        s->avg_freq_cache = 2;
        s->avg_io_cache = 4096;
    }

    if (s->io_congestion > 0) s->io_congestion = (s->io_congestion > 5) ? (s->io_congestion - 5) : 0;

    fc_update_averages_internal(s);
    spinlock_unlock(&s->lock);

    uint32_t dyn_flush_batch = s->total_entries / 4;
    if (dyn_flush_batch < 16) dyn_flush_batch = 16;
    if (dyn_flush_batch > 64) dyn_flush_batch = 64;

    file_cache_entry_t **victims = (file_cache_entry_t**)kmalloc(sizeof(file_cache_entry_t*) * dyn_flush_batch * 2);
    if (unlikely(!victims)) return;
    int vic_cnt = 0;

    // phase 1: 回收孤儿节点
    spinlock_lock(&s->lock);
    file_cache_entry_t *cur = s->lru_head;
    while (cur && (uint32_t)vic_cnt < dyn_flush_batch) { /* 修复: 符号比较 */
        file_cache_entry_t *next = cur->lru_next;
        if (unlikely(next && fc_bad_ptr(next))) break; /* 修复(#GP): 链上垃圾指针, 停止 */
        if (likely(next)) PREFETCH_R(next);   // 优化
        if (unlikely(cur->pending_reclaim && cur->pin_count == 0)) {
            void *art_val = art_delete(&s->index, cur->key, cur->key_len);
            if (likely(art_val)) {
                fc_lru_remove(s, cur);
                s->total_cache_bytes -= cur->data_len;
                if (cur->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= cur->data_len;
                if (cur->is_dirty) s->dirty_cache_bytes -= cur->data_len;
                s->total_cache_io    -= cur->total_io_len;
                s->total_cache_freq  -= cur->access_freq;
                s->total_entries--;
                if (cur->access_freq == 0) s->readahead_evictions++;
                victims[vic_cnt++] = cur;
            }
        }
        cur = next;
    }
    spinlock_unlock(&s->lock);
    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
    vic_cnt = 0;

    // phase 2: 刷脏页
    uint32_t max_flush = dyn_flush_batch;
    if (s->io_congestion >= 90) max_flush = 0;
    else if (s->io_congestion >= 70) max_flush = 1;
    else if (s->io_congestion >= 30) max_flush = dyn_flush_batch / 4;

    file_cache_entry_t **flush_list = NULL;
    uint32_t flush_cnt = 0;

    if (likely(max_flush > 0)) {  
        flush_list = (file_cache_entry_t**)kmalloc(sizeof(file_cache_entry_t*) * max_flush);
        if (unlikely(!flush_list)) { kfree(victims); return; }

        spinlock_lock(&s->lock);
        cur = s->lru_head;
        while (cur && flush_cnt < max_flush) {
            file_cache_entry_t *next = cur->lru_next;
            if (likely(next)) PREFETCH_R(next);   // 优化
            if (cur->is_dirty && cur->pin_count == 0 && cur->state == FC_STATE_CACHED && cur->writeback_retries < 5) {
                cur->state = FC_STATE_FLUSHING;
                cur->pin_count++;
                flush_list[flush_cnt++] = cur;
            }
            cur = next;
        }
        spinlock_unlock(&s->lock);

        if (likely(s->writeback_cb && flush_cnt > 0)) {
            for (uint32_t i = 0; i < flush_cnt; i++) {
                file_cache_entry_t *e = flush_list[i];
                int32_t rc = s->writeback_cb(e->key, e->key_len, e->data, e->data_len);
                if (likely(rc == 0)) {
                    e->writeback_retries = 0;
                    if (s->io_congestion > 0) s->io_congestion--;
                } else {
                    e->writeback_retries++;
                    s->io_congestion += 10;
                    if (s->io_congestion > 100) s->io_congestion = 100;
                    s->total_writeback_failures++;
                }
            }
        }

        spinlock_lock(&s->lock);
        for (uint32_t i = 0; i < flush_cnt; i++) {
            file_cache_entry_t *e = flush_list[i];
            e->pin_count--;
            e->state = (e->writeback_retries >= 5) ? FC_STATE_WRITEBACK_FAILED : FC_STATE_CACHED;
            if (e->writeback_retries == 0 && e->is_dirty) {
                e->is_dirty = false;
                s->dirty_cache_bytes -= e->data_len;
            }
        }
        spinlock_unlock(&s->lock);
    }

    // phase 3.5: 配额裁剪
    spinlock_lock(&s->lock);
    if (s->max_file_size > 0 && s->smoothed_cache_bytes > 0) {
        art_tree file_stats_tree;
        art_tree_init(&file_stats_tree);

        file_cache_entry_t *e_quota = s->lru_tail;
        uint32_t quota_scan_cnt = 0;
        uint32_t dyn_quota_batch = s->total_entries / 2;
        if (dyn_quota_batch < 64) dyn_quota_batch = 64;
        if (dyn_quota_batch > 512) dyn_quota_batch = 512;

        while (e_quota && quota_scan_cnt < dyn_quota_batch && (uint32_t)vic_cnt < dyn_flush_batch) {
            if (unlikely(fc_bad_ptr(e_quota))) break; /* 修复(#GP): lru_tail/链上垃圾指针 */
            file_cache_entry_t *prev = e_quota->lru_prev;
            if (likely(prev)) PREFETCH_R(prev);   // 优化: 从尾向头扫描预取前驱
            if (e_quota->file_id != 0 && e_quota->pin_count == 0 && e_quota->state == FC_STATE_CACHED && !e_quota->is_dirty) {
                uint64_t fid = e_quota->file_id;
                fc_file_stat_t *stat = (fc_file_stat_t *)art_search(&file_stats_tree, (const uint8_t*)&fid, sizeof(fid));
                if (!stat) {
                    stat = (fc_file_stat_t *)kmalloc(sizeof(fc_file_stat_t));
                    if (unlikely(!stat)) { e_quota = prev; quota_scan_cnt++; continue; }
                    stat->total_cached = 0;
                    stat->quota = (s->smoothed_cache_bytes * e_quota->file_size) / s->max_file_size;
                    art_insert(&file_stats_tree, (const uint8_t*)&fid, sizeof(fid), (void*)stat);
                }

                if (stat->total_cached + e_quota->data_len > stat->quota) {
                    bool is_oscillating = (e_quota->osc_count > 0 && s->avg_osc_cache > 0 && e_quota->osc_count > s->avg_osc_cache);
                    if (is_oscillating) {
                        stat->total_cached += e_quota->data_len;
                    } else {
                        void *art_val = art_delete(&s->index, e_quota->key, e_quota->key_len);
                        if (likely(art_val)) {
                            fc_lru_remove(s, e_quota);
                            s->total_cache_bytes -= e_quota->data_len;
                            if (e_quota->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= e_quota->data_len;
                            s->total_cache_io -= e_quota->total_io_len;
                            s->total_cache_freq -= e_quota->access_freq;
                            s->total_entries--;
                            s->evictions++;
                            if (e_quota->access_freq == 0) s->readahead_evictions++;
                            fc_update_oscillate(s, e_quota);
                            victims[vic_cnt++] = e_quota;
                        } else e_quota->pending_reclaim = true;
                    }
                } else stat->total_cached += e_quota->data_len;
            }
            e_quota = prev;
            quota_scan_cnt++;
        }
        art_iter(&file_stats_tree, fc_free_file_stat_cb, NULL);
        art_tree_destroy(&file_stats_tree);
    }
    spinlock_unlock(&s->lock);
    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
    vic_cnt = 0;

    // phase 3: 动态由最冷端向最热端判断
    spinlock_lock(&s->lock);
    cur = s->lru_head;
    int32_t scan_cnt = 0;
    uint64_t batch_max_size = 0;
    uint32_t dyn_reverse_scan = s->total_entries / 4;
    if (dyn_reverse_scan < 32) dyn_reverse_scan = 32;

    while (cur && (uint32_t)scan_cnt < dyn_reverse_scan && (uint32_t)vic_cnt < dyn_flush_batch) {
        file_cache_entry_t *next = cur->lru_next;
        if (unlikely(next && fc_bad_ptr(next))) break; /* 修复(#GP): 链上垃圾指针, 停止 */
        if (likely(next)) PREFETCH_R(next);   // 优化
        if (cur->file_size > batch_max_size) batch_max_size = cur->file_size;
        if (cur->pin_count == 0 && cur->state == FC_STATE_CACHED && !cur->is_dirty) {
            if (file_cache_should_evict(s, cur)) {
                void *art_val = art_delete(&s->index, cur->key, cur->key_len);
                if (likely(art_val)) {
                    fc_lru_remove(s, cur);
                    s->total_cache_bytes -= cur->data_len;
                    if (cur->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= cur->data_len;
                    s->total_cache_io    -= cur->total_io_len;
                    s->total_cache_freq  -= cur->access_freq;
                    s->total_entries--;
                    s->evictions++;
                    if (cur->access_freq == 0) s->readahead_evictions++;
                    fc_update_oscillate(s, cur);
                    victims[vic_cnt++] = cur;
                } else cur->pending_reclaim = true;
            }
        }
        cur = next;
        scan_cnt++;
    }

    if (batch_max_size > 0) s->max_file_size = (s->max_file_size * 7 + batch_max_size) / 8;

    /* [重建部分] 自适应扫描窗口反馈 (原代码计数器只收不用):
       持续 miss 说明当前窗口内的候选都不满足淘汰条件 → 扩大下轮窗口深挖;
       连续命中则回落到统计基线 (phase 0 已重算)。 */
    if (unlikely(s->evict_miss_count >= 3)) {
        s->evict_scan_window += s->evict_scan_window / 2;
        if (s->evict_scan_window > 512) s->evict_scan_window = 512;
        s->evict_miss_count = 0;
    }

    spinlock_unlock(&s->lock);
    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
    kfree(victims);

    if (flush_list) kfree(flush_list);
}

/* ==================== osc 树衰减 ==================== */
#define FC_OSC_DECAY_MAX_DEL 128   // 修复: 256→128, ctx 约 1.5KB, 控制中断上下文栈深

typedef struct {
    file_cache_cpu_t *s;
    const uint8_t* del_keys[FC_OSC_DECAY_MAX_DEL];
    uint32_t del_lens[FC_OSC_DECAY_MAX_DEL];
    uint32_t del_cnt;
} fc_osc_decay_ctx;

static int fc_osc_decay_cb(void *data, const uint8_t *key, uint32_t key_len, void *value) {
    fc_osc_decay_ctx *ctx = (fc_osc_decay_ctx *)data;
    fc_oscillate_t *osc = (fc_oscillate_t *)value;
    if (unlikely(!osc)) return 0;

    uint64_t old_osc = osc->osc_count;
    osc->osc_count >>= 1;
    ctx->s->total_oscillations -= (old_osc - osc->osc_count);   // 原版账目, 正确保留

    osc->freq >>= 1;
    osc->io_len >>= 1;

    if (unlikely(osc->osc_count == 0 && osc->freq == 0)) {
        if (likely(ctx->del_cnt < FC_OSC_DECAY_MAX_DEL)) {
            ctx->del_keys[ctx->del_cnt] = key;
            ctx->del_lens[ctx->del_cnt] = key_len;
            ctx->del_cnt++;
        }
    }
    /* 修复: 移除 ctx->batch >= 256 → return 1 的扫描上限 ——
       art_iter 每轮从最左叶子起步, 上限导致键序 257+ 的节点
       在前排名热不死时永远不被衰减, freq 只增不减;
       而 stats 回调无上限全树遍历, 均值被未衰减节点污染。 */
    return 0;
}

void file_cache_tick(file_cache_cpu_t *s) {
    if (unlikely(!s)) return;

    spinlock_lock(&s->lock);
    s->clock++;
    fc_update_averages_internal(s);

    uint32_t dyn_decay_ticks = (s->total_entries > 10000) ? 500 : 1000;

    if (unlikely(s->clock - s->last_decay_tick >= dyn_decay_ticks)) {
        s->last_decay_tick = s->clock;
        uint32_t batch = 0;
        uint32_t dyn_cache_decay_batch = s->total_entries / 8;
        if (dyn_cache_decay_batch < 32) dyn_cache_decay_batch = 32;
        if (dyn_cache_decay_batch > 256) dyn_cache_decay_batch = 256;

        if (unlikely(!s->decay_cursor)) s->decay_cursor = s->lru_head;
        if (likely(s->decay_cursor)) PREFETCH_RH(s->decay_cursor);   // 优化
        while (likely(s->decay_cursor && batch < dyn_cache_decay_batch)) {
            file_cache_entry_t *e = s->decay_cursor;
            s->decay_cursor = e->lru_next;
            if (likely(s->decay_cursor)) PREFETCH_R(s->decay_cursor);   // 优化

            s->total_cache_io    -= e->total_io_len;
            s->total_cache_freq  -= e->access_freq;
            /* 修复: 衰减下限钳 1 —— freq==1 时 >>= 变 0 会与预读冷数据标记混淆,
               且击穿 should_evict 的震荡保护 (freq==0 分支在保护检查之前) */
            if (likely(e->access_freq > 1)) e->access_freq >>= 1;
            e->total_io_len >>= 1;
            s->total_cache_io    += e->total_io_len;
            s->total_cache_freq  += e->access_freq;
            batch++;
        }

        fc_osc_decay_ctx ctx = {s, {0}, {0}, 0};   // 字段顺序随结构体调整更新
        art_iter(&s->oscillate_tree, fc_osc_decay_cb, &ctx);

        /* 原版账目自洽: 删除条件含 osc_count==0, 此处无需再扣 total_oscillations */
        for (uint32_t i = 0; i < ctx.del_cnt; i++) {
            void *val = art_delete(&s->oscillate_tree, ctx.del_keys[i], ctx.del_lens[i]);
            if (likely(val)) fc_oscillate_free(s, (fc_oscillate_t*)val);
        }
    }
    spinlock_unlock(&s->lock);
}


#pragma endregion