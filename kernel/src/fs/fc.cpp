// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <fs/fc.h>
#include <mem/heap.h>
#include <klib/algorithm/art.h>
#include <pdef.h>
#include <fs/fc_internal.h>

extern "C" void *__memcpy(void *d, const void *s, uint64_t n);
extern void  spinlock_lock(spinlock_t* lock);
extern void  spinlock_unlock(spinlock_t* lock);

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#endif




file_cache_cpu_t *g_fc_cpus[FC_MAX_CPUS];
uint32_t g_num_active_cpus = 0;
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

void fc_oscillate_free(file_cache_cpu_t *s, fc_oscillate_t *osc) {
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

int fc_collect_stats_cb(void *data, const uint8_t *key, uint32_t key_len, void *value) {
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

void fc_update_averages_internal(file_cache_cpu_t *s) {
    s->avg_osc_cache = s->oscillate_tree.size > 0 ? s->total_oscillations / s->oscillate_tree.size : 0;
    s->smoothed_cache_bytes = (s->smoothed_cache_bytes * 7 + s->total_cache_bytes) / 8;

    uint32_t dyn_window = __atomic_load_n(&s->total_entries, __ATOMIC_RELAXED) / 4;
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

bool file_cache_should_evict(file_cache_cpu_t *s, file_cache_entry_t *cur) {
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
bool fc_bad_ptr(const void *p) {
    return ((uint64_t)p >> 48) != 0xFFFF;
}

void fc_lru_remove(file_cache_cpu_t *s, file_cache_entry_t *e) {
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
/* 追踪器开关: 单线程套件开启; 多线程/TSAN 套件关闭(避免追踪器自身竞态) */
static int g_fc_track_on = 0;
extern "C" void fc_track_enable(int on) { g_fc_track_on = on; }
/* 独立释放日志开关 (无状态 printf, MT 诊断可用) */
static int g_fc_free_log = 0;
extern "C" void fc_free_log_enable(int on) { g_fc_free_log = on; }

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
    if (unlikely(!p || !g_fc_track_on)) return;
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
    if (unlikely(!e || !g_fc_track_on)) return;
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
    if (unlikely(!e || !g_fc_track_on)) return;
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

void fc_entry_free(file_cache_entry_t *e) {
    if (unlikely(!e)) return;
#ifdef __KERNEL_TEST_HOST__
    if (g_fc_free_log)
        printf_("[FCFREE] e=%p ra=0x%llx\n", (void*)e,
                (unsigned long long)(uintptr_t)__builtin_return_address(0));
#endif
#ifdef __KERNEL_TEST_HOST__
    if (g_test_op == 565) printf_("[P565] fc_entry_free e=%p key=%p\n", (void*)e, (void*)e->key);
#endif
#ifdef __KERNEL_TEST_HOST__
    /* 释放时仍在其所属缓存的 ART 中 = 悬空指针 bug, 直接打印释放站点 */
    if (g_fc_track_on && e->cpu_id < 64 && g_fc_cpus[e->cpu_id] &&
        art_search(&g_fc_cpus[e->cpu_id]->index, e->key, e->key_len) == e) {
        printf_("[FCDBG] FREE WHILE STILL IN ART: entry %p (key_len=%u) ra=0x%llx\n",
                (void*)e, e->key_len, (unsigned long long)__builtin_return_address(0));
    }
#endif
    if (e->data && e->data != e->inline_data) kfree(e->data);
    if (e->key) kfree(e->key);
#ifdef __KERNEL_TEST_HOST__
    fc_track_free(e, (uintptr_t)__builtin_return_address(0));
#endif
    kfree(e);
    /* 注: 原 ASAN 毒化插桩已移除 —— SLUB 空闲链指针就存在已释放对象的
       头 8 字节里, 毒化会与下一次 pop 的链读取自伤; 悬垂/双释由
       fc_track_* 与 art 完整性巡检覆盖 */
}

void fc_update_oscillate(file_cache_cpu_t *s, file_cache_entry_t *v) {
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
            if (unlikely(av == NULL)) { continue; } /* 修复(#GP): 已摘除, 勿双删 —— round 98 静态分析: 原 cur=cur->lru_next 与循环步进重复前进, 跳过一节点且尾端可 NULL 解引用 */
            fc_lru_remove(s, cur);
            s->total_cache_bytes -= cur->data_len;
            if (cur->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= cur->data_len;
            s->total_cache_io    -= cur->total_io_len;
            s->total_cache_freq  -= cur->access_freq;
            __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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
                    __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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
            __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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
                        int32_t (*writeback_cb)(uint64_t, const uint8_t*, uint32_t, void*, size_t)) {
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
    __atomic_store_n(&s->total_entries, 0, __ATOMIC_RELAXED);
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
            } else if (e->is_dirty && s->writeback_cb &&
                       e->state != FC_STATE_WRITEBACK_FAILED) {
                /* P1-34: 脏条目先写回再摘除 (原实现直接删除 = 静默丢
                   脏页)。writeback_cb 做磁盘 I/O, 不得持锁调用 ——
                   借用 fsync 的 FLUSHING + pin 模式。 */
                e->state = FC_STATE_FLUSHING; e->pin_count++;
                spinlock_unlock(&s->lock);
                int32_t wb_rc = s->writeback_cb(e->file_id, e->key, e->key_len, e->data, e->data_len);
                spinlock_lock(&s->lock);
                e->pin_count--;
                /* 审计 #3 (round 1 修复): 原实现丢弃写回返回值, 失败也删
                   条目 = 静默丢脏数据。失败 → 转 WRITEBACK_FAILED
                   (30s 冷却重试机制接管), 不删脏页 */
                if (unlikely(wb_rc != 0)) {
                    e->state = FC_STATE_WRITEBACK_FAILED;
                    e->writeback_retries = 5;
                    s->total_writeback_failures++;
                    spinlock_unlock(&s->lock);
                    continue;
                }
                void *art_val = art_delete(&s->index, e->key, e->key_len);
                if (likely(art_val)) {
                    fc_lru_remove(s, e);
                    s->total_cache_bytes -= e->data_len;
                    if (e->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= e->data_len;
                    if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
                    s->total_cache_io    -= e->total_io_len;
                    s->total_cache_freq  -= e->access_freq;
                    __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
                    entry_to_free = e;
                } else {
                    e->pending_reclaim = true;
                }
            } else {
                void *art_val = art_delete(&s->index, e->key, e->key_len);
                if (likely(art_val)) {
                    fc_lru_remove(s, e);
                    s->total_cache_bytes -= e->data_len;
                    if (e->data_len < FC_TINY_FILE_THRESHOLD) s->tiny_cache_bytes -= e->data_len;
                    if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
                    s->total_cache_io    -= e->total_io_len;
                    s->total_cache_freq  -= e->access_freq;
                    __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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

    uint32_t dyn_migrate_batch = __atomic_load_n(&src->total_entries, __ATOMIC_RELAXED) / 16;
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
    while (cur && (uint32_t)migrated < dyn_migrate_batch && (uint32_t)scanned < __atomic_load_n(&src->total_entries, __ATOMIC_RELAXED)) {
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
                __atomic_fetch_sub(&src->total_entries, 1, __ATOMIC_RELAXED);
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
        __atomic_fetch_sub(&src->total_entries, 1, __ATOMIC_RELAXED);
        src->migrations_out++;

        cur->cpu_id = best_dst;
        art_insert(&dst->index, cur->key, cur->key_len, (void *)cur);
#ifdef __KERNEL_TEST_HOST__
        fc_track_insert(cur, "check_load");
#endif
        fc_lru_push_back(dst, cur);

        __atomic_fetch_add(&dst->total_entries, 1, __ATOMIC_RELAXED);
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

/* 打开实例编号: 每次 fopen 取一个新号, 用作块缓存的 file_id。
   0 保留 (fc.cpp 内部多处以 file_id==0 表示"无效")。 */
uint64_t fc_next_file_uid(void) {
    static uint64_t s_next = 1;
    return __atomic_add_fetch(&s_next, 1, __ATOMIC_RELAXED);
}

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
                __atomic_fetch_sub(&rs->total_entries, 1, __ATOMIC_RELAXED);
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
                __atomic_fetch_add(&s->total_entries, 1, __ATOMIC_RELAXED);
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
                __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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
            __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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
                    __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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
    __atomic_fetch_add(&s->total_entries, 1, __ATOMIC_RELAXED);
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

    __atomic_fetch_add(&s->total_entries, 1, __ATOMIC_RELAXED);
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

/* ---- 块级缓存实现 (Step 1, round 26): 键 = (file_id, block#) ----
   本阶段为整条目语义的键构造层 (fc-block-cache-design.md); fops 尚未
   逐块调用, 行为与 path 版等价, 后续 Step 2/3 切换调用点后获得块粒度 */
static inline void fc_block_key(uint64_t file_id, uint64_t block,
                                uint8_t out[16]) {
    __memcpy(out, &file_id, 8);
    __memcpy(out + 8, &block, 8);
}

void* file_cache_get_block(file_cache_cpu_t *s, uint64_t file_id, uint64_t block,
                           size_t io_len, size_t *out_len,
                           file_cache_entry_t **out_entry) {
    if (unlikely(!s || file_id == 0)) return nullptr;
    uint8_t key[16];
    fc_block_key(file_id, block, key);
    return file_cache_get(s, key, 16, io_len, out_len, out_entry);
}

int32_t file_cache_promote_block(file_cache_cpu_t *s, uint64_t file_id, uint64_t block,
                                 void *data, size_t data_len, bool is_dirty,
                                 uint64_t file_size) {
    if (unlikely(!s || file_id == 0)) return -1;
    uint8_t key[16];
    fc_block_key(file_id, block, key);
    return file_cache_promote(s, key, 16, data, data_len, is_dirty,
                              file_size, file_id);
}

void file_cache_invalidate_block(file_cache_cpu_t *s, uint64_t file_id, uint64_t block) {
    if (unlikely(!s || file_id == 0)) return;
    uint8_t key[16];
    fc_block_key(file_id, block, key);
    fc_broadcast_invalidate(s, key, 16);
}

/* Step 4 (round 29/30): 按 file_id 失效整个文件的块级条目 ——
   O_TRUNC/文件删除时调用 (设计 §4: 区间前缀删除; 本实现按已知
   文件块数逐块失效, 块数 = ceil(file_size/4096), 有界)。
   fc_broadcast_invalidate 跳过 src (调用方语义为"其他核"), 本机
   条目在此显式删除 —— round 30 宿主回归实锤该缺口 */
void file_cache_invalidate_file(file_cache_cpu_t *s, uint64_t file_id,
                                uint64_t file_size) {
    if (unlikely(!s || file_id == 0)) return;
    uint64_t blocks = (file_size + 4095) / 4096;
    if (blocks == 0) blocks = 1;
    for (uint64_t b = 0; b < blocks; b++) {
        uint8_t key[16];
        fc_block_key(file_id, b, key);
        fc_broadcast_invalidate(s, key, 16);
        /* 本机条目显式摘除 (广播只覆盖其他核) */
        spinlock_lock(&s->lock);
        file_cache_entry_t *e = (file_cache_entry_t *)art_search(
            &s->index, key, 16);
        if (e && e->pin_count == 0) {
            void *art_val = art_delete(&s->index, key, 16);
            if (likely(art_val)) {
                fc_lru_remove(s, e);
                s->total_cache_bytes -= e->data_len;
                if (e->data_len < FC_TINY_FILE_THRESHOLD)
                    s->tiny_cache_bytes -= e->data_len;
                if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
                s->total_cache_io -= e->total_io_len;
                s->total_cache_freq -= e->access_freq;
                __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
                spinlock_unlock(&s->lock);
                kfree(e->data);
                kfree(e);
                continue;
            }
        } else if (e) {
            e->state = FC_STATE_INVALID;   /* pin 中: 标记, 借完删除 */
        }
        spinlock_unlock(&s->lock);
    }
}

#pragma endregion
