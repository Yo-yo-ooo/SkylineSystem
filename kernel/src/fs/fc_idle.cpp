// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
// fc_idle.cpp - 文件缓存后台维护与 fsync (拆分自 fc.cpp)
//   idle_handler / tick / 写回冲刷 / fsync / 淘汰扫描
#include <fs/fc.h>
#include <mem/heap.h>
#include <klib/algorithm/art.h>
#include <pdef.h>
#ifdef __x86_64__
#include <arch/x86_64/pit/pit.h>
/* 冷却重试的墙钟来源: 内核 = PIT 毫秒 */
static inline uint64_t fc_wall_ms(void) { return PIT::TimeSinceBootMS(); }
#else
/* 宿主单元测试 (tests/fc 直接链 fc 源码) 无 PIT: 时间冻结,
   冷却重试在测试中不触发 (测试不模拟 30s 墙钟) */
static inline uint64_t fc_wall_ms(void) { return 0; }
#endif

#include <fs/fc_internal.h>

extern "C" void *__memcpy(void *d, const void *s, uint64_t n);
extern void  spinlock_lock(spinlock_t* lock);
extern void  spinlock_unlock(spinlock_t* lock);

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

    uint32_t dyn_flush_batch = __atomic_load_n(&s->total_entries, __ATOMIC_RELAXED) / 4;
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
                __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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
        uint64_t now_tick = fc_wall_ms();   /* P1-35: 冷却重试时间戳 */
        while (cur && flush_cnt < max_flush) {
            file_cache_entry_t *next = cur->lru_next;
            if (likely(next)) PREFETCH_R(next);   // 优化
            /* P1-35: WRITEBACK_FAILED 条目按冷却期重试 —— 原过滤条件
               writeback_retries<5 让 FAILED 条目永久不再尝试 (条目滞留
               且脏数据永远写不回)。30s 冷却后重试, 成功即清零重试计数。 */
            bool failed_retry = (cur->state == FC_STATE_WRITEBACK_FAILED &&
                                 cur->is_dirty && cur->pin_count == 0 &&
                                 now_tick - cur->last_access_tick > 30000);
            if (cur->is_dirty && cur->pin_count == 0 &&
                ((cur->state == FC_STATE_CACHED && cur->writeback_retries < 5) ||
                 failed_retry)) {
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
        uint32_t dyn_quota_batch = __atomic_load_n(&s->total_entries, __ATOMIC_RELAXED) / 2;
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
                            __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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
    uint32_t dyn_reverse_scan = __atomic_load_n(&s->total_entries, __ATOMIC_RELAXED) / 4;
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
                    __atomic_fetch_sub(&s->total_entries, 1, __ATOMIC_RELAXED);
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
        uint32_t dyn_cache_decay_batch = __atomic_load_n(&s->total_entries, __ATOMIC_RELAXED) / 8;
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
