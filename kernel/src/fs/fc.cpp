// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <fs/fc.h>
#include <mem/heap.h>
#include <klib/algorithm/art.h>

extern "C" void *__memcpy(void *d, const void *s, uint64_t n);
extern void  spinlock_lock(spinlock_t* lock);
extern void  spinlock_unlock(spinlock_t* lock);

// 内部静态注册表
static file_cache_cpu_t *g_fc_cpus[FC_MAX_CPUS];
static uint32_t g_num_active_cpus = 0;
static spinlock_t g_fc_init_lock = 0;

/* ===================== Heuristic Strategy ===================== */
static void fc_update_averages_internal(file_cache_cpu_t *s) {
    if (s->total_entries > 0) {
        s->avg_io_cache = s->total_cache_io / s->total_entries;
    } else {
        s->avg_io_cache = 0;
    }
    
    if (s->total_cache_bytes > 0) {
        s->avg_freq_cache = s->total_cache_freq / s->total_cache_bytes;
    } else {
        s->avg_freq_cache = 0;
    }

    if (s->oscillate_tree.size > 0) {
        s->avg_osc_cache = s->total_oscillations / s->oscillate_tree.size;
    } else {
        s->avg_osc_cache = 0;
    }
}

static bool file_cache_should_cache(file_cache_cpu_t *s, uint64_t freq, uint64_t target_cache_len, uint64_t osc_count) {
    if (osc_count > 0 && s->avg_osc_cache > 0) {
        if (osc_count > s->avg_osc_cache) return true; 
    }

    if (freq < FC_MIN_FREQ_TO_CACHE) return false;
    if (target_cache_len < FC_MIN_IO_LEN_TO_CACHE) return false;
    
    bool cond1 = (target_cache_len > s->avg_io_cache);
    bool cond2 = true;
    if (s->avg_freq_cache > 0) {
        cond2 = (freq > s->avg_freq_cache * target_cache_len);
    }
    return cond1 && cond2;
}

static bool file_cache_should_evict(file_cache_cpu_t *s, file_cache_entry_t *cur) {
    if (cur->osc_count > 0 && s->avg_osc_cache > 0) {
        if (cur->osc_count > s->avg_osc_cache) return false; 
    }

    if (s->clock - cur->create_tick < FC_MIN_RESIDENCE_TICKS) return false;

    bool cond1 = (s->avg_io_cache > 0) && (cur->total_io_len < s->avg_io_cache);
    bool cond2 = (s->avg_freq_cache > 0) && (cur->access_freq < s->avg_freq_cache * cur->data_len);
    return cond1 || cond2;
}

/* ===================== LRU Operations ===================== */
static void fc_lru_remove(file_cache_cpu_t *s, file_cache_entry_t *e) {
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else             s->lru_head = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else             s->lru_tail = e->lru_prev;
    if (s->decay_cursor == e) s->decay_cursor = e->lru_next;
    e->lru_prev = e->lru_next = NULL;
}

static void fc_lru_push_back(file_cache_cpu_t *s, file_cache_entry_t *e) {
    e->lru_next = NULL;
    e->lru_prev = s->lru_tail;
    if (s->lru_tail) s->lru_tail->lru_next = e;
    else              s->lru_head = e;
    s->lru_tail = e;
}

static void fc_lru_move_to_back(file_cache_cpu_t *s, file_cache_entry_t *e) {
    fc_lru_remove(s, e);
    fc_lru_push_back(s, e);
}

static file_cache_entry_t *fc_pick_victim(file_cache_cpu_t *s) {
    file_cache_entry_t *clean_fallback = NULL;
    int32_t scan_cnt = 0;
    bool hit = false;
    
    for (file_cache_entry_t *cur = s->lru_head; cur && scan_cnt < s->evict_scan_window; cur = cur->lru_next) {
        if (cur->pending_reclaim && cur->pin_count == 0) return cur;
        
        if (cur->pin_count == 0 && cur->state == FC_STATE_CACHED && !cur->is_dirty) {
            bool is_protected = (cur->osc_count > 0 && s->avg_osc_cache > 0 && cur->osc_count > s->avg_osc_cache);
            bool is_young = (s->clock - cur->create_tick < FC_MIN_RESIDENCE_TICKS);
            
            if (!is_protected && !is_young) {
                if (!clean_fallback) clean_fallback = cur;
            }
            
            if (file_cache_should_evict(s, cur)) {
                hit = true;
                s->evict_hit_count++;
                s->evict_miss_count = 0;
                if (s->evict_hit_count >= s->evict_hit_threshold) {
                    if (s->evict_scan_window > FC_EVICT_WINDOW_MIN) {
                        s->evict_scan_window = s->evict_scan_window * 3 / 4;
                        if (s->evict_scan_window < FC_EVICT_WINDOW_MIN) s->evict_scan_window = FC_EVICT_WINDOW_MIN;
                    }
                    s->evict_hit_count = 0;
                }
                return cur; 
            }
            scan_cnt++;
        }
    }
    
    if (!hit) {
        s->evict_hit_count = 0;
        s->evict_miss_count++;
        if (s->evict_miss_count >= 8) s->evict_hit_threshold = FC_EVICT_HIT_THRESHOLD * 2;
        if (s->evict_scan_window < FC_EVICT_WINDOW_MAX) {
            s->evict_scan_window = s->evict_scan_window * 5 / 4;
            if (s->evict_scan_window > FC_EVICT_WINDOW_MAX) s->evict_scan_window = FC_EVICT_WINDOW_MAX;
        }
    }
    return clean_fallback; 
}

static void fc_entry_free(file_cache_entry_t *e) {
    if (!e) return;
    if (e->data) kfree(e->data);
    if (e->key)  kfree(e->key);
    kfree(e);
}

static int32_t fc_evict_one(file_cache_cpu_t *s) {
    file_cache_entry_t *v = fc_pick_victim(s);
    if (!v) return -1;

    if (v->pending_reclaim) {
        fc_lru_remove(s, v);
        s->total_cache_bytes -= v->data_len;
        s->total_cache_io    -= v->total_io_len;
        s->total_cache_freq  -= v->access_freq;
        s->total_entries--;
        s->evictions++;
        fc_entry_free(v);
        return 0;
    }

    v->state = FC_STATE_EVICTING;
    void *art_val = art_delete(&s->index, v->key, v->key_len);
    if (!art_val) {
        v->state = FC_STATE_CACHED;
        v->pending_reclaim = true; 
        return -1; 
    }
    fc_lru_remove(s, v);

    s->total_cache_bytes -= v->data_len;
    s->total_cache_io    -= v->total_io_len;
    s->total_cache_freq  -= v->access_freq;
    s->total_entries--;
    s->evictions++;
    
    fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, v->key, v->key_len);
    if (!osc) {
        osc = (fc_oscillate_t *)kmalloc(sizeof(fc_oscillate_t));
        if (osc) {
            osc->freq = v->access_freq;
            osc->io_len = v->total_io_len;
            osc->osc_count = v->osc_count + 1;
            if (osc->osc_count > FC_OSC_COUNT_LIMIT) osc->osc_count = FC_OSC_COUNT_LIMIT;
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
        if (osc->osc_count > FC_OSC_COUNT_LIMIT) osc->osc_count = FC_OSC_COUNT_LIMIT;
        s->total_oscillations += osc->osc_count;
        if (v->file_size > 0) osc->file_size = v->file_size;
        osc->file_id = v->file_id;
    }
    
    fc_entry_free(v);
    return 0;
}

/* ===================== Init ===================== */
void file_cache_cpu_init(file_cache_cpu_t *s, uint32_t cpu_id, 
                        uint32_t max_entries, uint64_t max_mem,
                        void (*writeback_cb)(const uint8_t*, uint32_t, void*, size_t)) {
    if (!s) return;
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
    s->avg_io_cache = 0; s->avg_freq_cache = 0; s->avg_osc_cache = 0;
    s->total_entries = 0;
    s->max_entries = max_entries ? max_entries : FC_DEFAULT_MAX_ENTRIES;
    s->base_memory = max_mem ? max_mem : FC_DEFAULT_MAX_MEMORY;
    s->evict_scan_window = 64;
    s->evict_hit_count = 0; s->evict_miss_count = 0; s->evict_hit_threshold = FC_EVICT_HIT_THRESHOLD;
    s->hits = 0; s->misses = 0; s->evictions = 0;
    s->migrations_in = 0; s->migrations_out = 0;
    s->writeback_cb = writeback_cb;

    // 修复竞态：使用锁保护全局注册表
    spinlock_lock(&g_fc_init_lock);
    if (cpu_id < FC_MAX_CPUS) {
        g_fc_cpus[cpu_id] = s;
        if (cpu_id + 1 > g_num_active_cpus) g_num_active_cpus = cpu_id + 1;
    }
    spinlock_unlock(&g_fc_init_lock);
}

/* ===================== Broadcast Invalidation ===================== */
static void fc_broadcast_invalidate(file_cache_cpu_t *src_s, const uint8_t *key, uint32_t key_len) {
    for (uint32_t i = 0; i < g_num_active_cpus; i++) {
        if (i == src_s->cpu_id) continue;
        file_cache_cpu_t *s = g_fc_cpus[i];
        if (!s) continue;
        
        spinlock_lock(&s->lock);
        file_cache_entry_t *e = (file_cache_entry_t *)art_search(&s->index, key, key_len);
        if (e) {
            if (e->pin_count > 0) {
                e->state = FC_STATE_INVALID;
            } else {
                void *art_val = art_delete(&s->index, e->key, e->key_len);
                if (art_val) {
                    fc_lru_remove(s, e);
                    s->total_cache_bytes -= e->data_len;
                    if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
                    s->total_cache_io    -= e->total_io_len;
                    s->total_cache_freq  -= e->access_freq;
                    s->total_entries--;
                    fc_entry_free(e);
                } else {
                    e->pending_reclaim = true;
                }
            }
        }
        fc_oscillate_t *osc = (fc_oscillate_t *)art_delete(&s->oscillate_tree, key, key_len);
        if (osc) {
            s->total_oscillations -= osc->osc_count;
            kfree(osc);
        }
        spinlock_unlock(&s->lock);
    }
}

/* ===================== Get / Put ===================== */
void *file_cache_get(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                     size_t io_len, size_t *out_len, file_cache_entry_t **out_entry) {
    if (!s || !key || key_len == 0) return NULL;

    spinlock_lock(&s->lock);
    file_cache_entry_t *e = (file_cache_entry_t *)art_search(&s->index, key, key_len);

    if (e) {
        s->hits++;
        e->access_freq++;
        e->total_io_len += io_len;
        s->total_cache_io += io_len;
        s->total_cache_freq++;
        fc_lru_move_to_back(s, e);
        e->pin_count++;
        if (out_len) *out_len = e->data_len;
        if (out_entry) *out_entry = e;
        void *data = e->data;
        spinlock_unlock(&s->lock);
        return data;
    }
    s->misses++;
    spinlock_unlock(&s->lock);

    for (uint32_t i = s->cpu_id + 1; i < g_num_active_cpus; i++) {
        file_cache_cpu_t *rs = g_fc_cpus[i];
        if (!rs) continue;
        
        spinlock_lock(&rs->lock);
        file_cache_entry_t *re = (file_cache_entry_t *)art_search(&rs->index, key, key_len);
        if (re && !re->is_dirty && re->pin_count == 0 && re->state == FC_STATE_CACHED) {
            void *art_val = art_delete(&rs->index, re->key, re->key_len);
            if (art_val) {
                fc_lru_remove(rs, re);
                rs->total_cache_bytes -= re->data_len;
                rs->total_cache_io    -= re->total_io_len;
                rs->total_cache_freq  -= re->access_freq;
                rs->total_entries--;
                rs->migrations_out++;
                spinlock_unlock(&rs->lock);
                
                spinlock_lock(&s->lock);
                // 修复：纯本地配额硬上限检查
                if (s->total_entries + 1 > s->max_entries || s->total_cache_bytes + re->data_len > s->base_memory) {
                    // 本地配额已满，放回原 CPU
                    spinlock_unlock(&s->lock);
                    spinlock_lock(&rs->lock);
                    art_insert(&rs->index, re->key, re->key_len, (void *)re);
                    fc_lru_push_back(rs, re);
                    rs->total_entries++;
                    rs->total_cache_bytes += re->data_len;
                    rs->total_cache_io += re->total_io_len;
                    rs->total_cache_freq += re->access_freq;
                    spinlock_unlock(&rs->lock);
                    return NULL;
                }
                
                re->cpu_id = s->cpu_id;
                art_insert(&s->index, re->key, re->key_len, (void *)re);
                fc_lru_push_back(s, re);
                
                s->total_entries++;
                s->total_cache_bytes += re->data_len;
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
    if (!s || !e) return;
    
    spinlock_lock(&s->lock);
    if (e->pin_count > 0) e->pin_count--;

    if (e->pin_count == 0 && (e->state == FC_STATE_INVALID || e->pending_reclaim)) {
        if (!e->pending_reclaim) {
            void *art_val = art_delete(&s->index, e->key, e->key_len);
            if (!art_val) {
                e->pending_reclaim = true;
                spinlock_unlock(&s->lock);
                return;
            }
        }
        fc_lru_remove(s, e);
        s->total_cache_bytes -= e->data_len;
        if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
        s->total_cache_io    -= e->total_io_len;
        s->total_cache_freq  -= e->access_freq;
        s->total_entries--;
        fc_entry_free(e);
    }
    spinlock_unlock(&s->lock);
}
int32_t file_cache_promote_locked(file_cache_cpu_t *s,
                           const uint8_t *key, uint32_t key_len,
                           void *data, size_t data_len, bool is_dirty, uint64_t file_size, uint64_t file_id, bool *out_need_broadcast);
/* ===================== Record IO & Promote ===================== */
int32_t file_cache_record_io(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                             size_t io_len, void *data_if_promote, uint64_t file_size, uint64_t file_id) {
    if (!s || !key || key_len == 0 || io_len == 0) return -1;

    spinlock_lock(&s->lock);
    
    if (file_size > 0 && file_size > s->max_file_size) {
        s->max_file_size = file_size;
    }

    file_cache_entry_t *e = (file_cache_entry_t *)art_search(&s->index, key, key_len);

    if (e) {
        e->access_freq++;
        e->total_io_len += io_len;
        if (file_size > 0) e->file_size = file_size;
        if (file_id != 0) e->file_id = file_id;
        s->total_cache_io += io_len;
        s->total_cache_freq++;
        fc_lru_move_to_back(s, e);
        spinlock_unlock(&s->lock);
        return 0;
    }

    fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, key, key_len);
    uint64_t cur_freq = osc ? osc->freq + 1 : 1;
    uint64_t cur_io   = osc ? osc->io_len + io_len : io_len;
    uint64_t cur_osc  = osc ? osc->osc_count : 0;
    uint64_t cur_fsize = osc ? osc->file_size : file_size;
    uint64_t cur_fid   = osc ? osc->file_id : file_id;

    if (cur_fsize > 0 && cur_fsize > s->max_file_size) {
        s->max_file_size = cur_fsize;
    }

    bool should = file_cache_should_cache(s, cur_freq, io_len, cur_osc);
    if (should && data_if_promote) {
        bool need_broadcast = false;
        int32_t r = file_cache_promote_locked(s, key, key_len, data_if_promote, io_len, false, cur_fsize, cur_fid, &need_broadcast);
        if (r != 0) {
            if (!osc) {
                osc = (fc_oscillate_t *)kmalloc(sizeof(fc_oscillate_t));
                if (osc) {
                    osc->freq = 1; osc->io_len = io_len; osc->osc_count = 0; 
                    osc->file_size = file_size; osc->file_id = file_id;
                    art_insert(&s->oscillate_tree, key, key_len, (void *)osc);
                }
            } else {
                osc->freq++; osc->io_len += io_len;
                if (file_size > 0) osc->file_size = file_size;
                if (file_id != 0) osc->file_id = file_id;
            }
        } else {
            if (osc) {
                s->total_oscillations -= osc->osc_count;
                art_delete(&s->oscillate_tree, key, key_len);
                kfree(osc);
            }
        }
        spinlock_unlock(&s->lock);
        if (r != 0) kfree(data_if_promote);
        return (r == 0) ? (int32_t)io_len : r;
    } else {
        if (!osc) {
            osc = (fc_oscillate_t *)kmalloc(sizeof(fc_oscillate_t));
            if (osc) {
                osc->freq = 1; osc->io_len = io_len; osc->osc_count = 0; 
                osc->file_size = file_size; osc->file_id = file_id;
                art_insert(&s->oscillate_tree, key, key_len, (void *)osc);
            }
        } else {
            osc->freq++; osc->io_len += io_len;
            if (file_size > 0) osc->file_size = file_size;
            if (file_id != 0) osc->file_id = file_id;
        }
        spinlock_unlock(&s->lock);
        if (!should && data_if_promote) kfree(data_if_promote); 
        return 0;
    }
}

int32_t file_cache_promote_locked(file_cache_cpu_t *s,
                           const uint8_t *key, uint32_t key_len,
                           void *data, size_t data_len, bool is_dirty, uint64_t file_size, uint64_t file_id, bool *out_need_broadcast) {
    *out_need_broadcast = false;
    file_cache_entry_t *exist = (file_cache_entry_t *)art_search(&s->index, key, key_len);
    if (exist) {
        if (exist->data) kfree(exist->data);
        s->total_cache_bytes -= exist->data_len;
        if (exist->is_dirty) s->dirty_cache_bytes -= exist->data_len;
        s->total_cache_io    -= exist->total_io_len;
        
        exist->data = data;
        exist->data_len = data_len;
        exist->total_io_len += data_len; 
        exist->is_dirty = is_dirty;
        if (file_size > 0) exist->file_size = file_size;
        if (file_id != 0) exist->file_id = file_id;
        
        s->total_cache_bytes += data_len;
        if (is_dirty) s->dirty_cache_bytes += data_len;
        s->total_cache_io += exist->total_io_len;
        fc_lru_move_to_back(s, exist);
        
        if (is_dirty) *out_need_broadcast = true;
        return 0;
    }

    if (is_dirty && s->total_cache_bytes > 0 && 
        (s->dirty_cache_bytes * 100 / s->total_cache_bytes) > FC_DIRTY_RATIO_LIMIT) {
        return FC_ERR_NO_MEMORY; 
    }

    int32_t evict_cnt = 0;
    bool need_evict = (s->total_entries + 1 > s->max_entries) ||
                      (s->total_cache_bytes + data_len > s->base_memory);
                      
    while (need_evict && evict_cnt < 8) {
        if (fc_evict_one(s) != 0) break;
        need_evict = (s->total_entries + 1 > s->max_entries) ||
                     (s->total_cache_bytes + data_len > s->base_memory);
        evict_cnt++;
    }

    if (s->total_entries + 1 > s->max_entries || s->total_cache_bytes + data_len > s->base_memory) {
        kfree(data); return FC_ERR_NO_MEMORY;
    }

    file_cache_entry_t *e = (file_cache_entry_t *)kcalloc(1, sizeof(file_cache_entry_t));
    if (!e) { kfree(data); return -3; }

    e->key = (uint8_t *)kmalloc(key_len);
    if (!e->key) { kfree(e); kfree(data); return -3; }
    
    __memcpy(e->key, key, key_len);
    e->key_len = key_len;
    e->cpu_id = s->cpu_id;
    e->pending_reclaim = false;
    e->is_dirty = is_dirty;
    e->file_size = file_size;
    e->file_id = file_id;

    e->data = data;
    e->data_len = data_len;
    e->state = FC_STATE_CACHED;
    e->pin_count = 0;
    e->create_tick = s->clock;
    e->last_access_tick = s->clock;
    e->total_io_len = data_len;
    
    fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, key, key_len);
    if (osc) {
        e->access_freq = osc->freq;
        e->osc_count = osc->osc_count;
    } else {
        e->access_freq = 1;
        e->osc_count = 0;
    }

    art_insert(&s->index, e->key, e->key_len, (void *)e);
    if (art_search(&s->index, e->key, e->key_len) != e) {
        kfree(e->key); kfree(e); kfree(data); return -4;
    }

    fc_lru_push_back(s, e);
    s->total_entries++;
    s->total_cache_bytes += data_len;
    if (is_dirty) s->dirty_cache_bytes += data_len;
    s->total_cache_io    += e->total_io_len;
    s->total_cache_freq  += e->access_freq;

    if (is_dirty) *out_need_broadcast = true;
    return 0;
}

int32_t file_cache_promote(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                           void *data, size_t data_len, bool is_dirty, uint64_t file_size, uint64_t file_id) {
    if (!s || !key || key_len == 0 || !data || data_len == 0) return -1;
    
    bool need_broadcast = false;

    spinlock_lock(&s->lock);
    if (file_size > 0 && file_size > s->max_file_size) {
        s->max_file_size = file_size;
    }

    int32_t r = file_cache_promote_locked(s, key, key_len, data, data_len, is_dirty, file_size, file_id, &need_broadcast);
    spinlock_unlock(&s->lock);

    if (r == 0 && need_broadcast) {
        fc_broadcast_invalidate(s, key, key_len);
    } else if (r != 0) {
        kfree(data);
    }
    
    return r;
}

int32_t file_cache_invalidate(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len) {
    if (!s || !key || key_len == 0) return -1;
    fc_broadcast_invalidate(s, key, key_len);
    return 0;
}

/* ===================== CPU Load Check & Migration ===================== */
void file_cache_check_load(file_cache_cpu_t *src, uint32_t load_factor) {
    if (!src || load_factor < 80) return;
    
    uint32_t best_dst = -1;
    uint32_t lowest_load = 100;
    for (uint32_t i = 0; i < g_num_active_cpus; i++) {
        if (i == src->cpu_id || !g_fc_cpus[i]) continue;
        file_cache_cpu_t *dst = g_fc_cpus[i];
        if (dst->total_entries < dst->max_entries) {
            uint32_t proxy_load = (dst->total_entries * 100) / dst->max_entries;
            if (proxy_load < lowest_load) {
                lowest_load = proxy_load;
                best_dst = i;
            }
        }
    }
    if (best_dst == (uint32_t)-1) return;

    file_cache_cpu_t *dst = g_fc_cpus[best_dst];

    if (src->cpu_id < best_dst) {
        spinlock_lock(&src->lock);
        spinlock_lock(&dst->lock);
    } else {
        spinlock_lock(&dst->lock);
        spinlock_lock(&src->lock);
    }

    int32_t migrated = 0;
    while (migrated < FC_MIGRATE_BATCH_SIZE && src->lru_head) {
        file_cache_entry_t *e = src->lru_head;
        if (e->pin_count > 0 || e->state != FC_STATE_CACHED || e->is_dirty) {
            fc_lru_move_to_back(src, e); 
            continue;
        }

        // 纯本地配额检查
        if (dst->total_entries + 1 > dst->max_entries || dst->total_cache_bytes + e->data_len > dst->base_memory) {
            break; // 目标满了
        }

        fc_oscillate_t *osc = (fc_oscillate_t *)art_delete(&src->oscillate_tree, e->key, e->key_len);
        if (osc) {
            src->total_oscillations -= osc->osc_count;
        }

        if (art_search(&dst->index, e->key, e->key_len) != NULL) {
            void *art_val = art_delete(&src->index, e->key, e->key_len);
            if (art_val) {
                fc_lru_remove(src, e);
                src->total_cache_bytes -= e->data_len;
                src->total_cache_io    -= e->total_io_len;
                src->total_cache_freq  -= e->access_freq;
                src->total_entries--;
                src->migrations_out++;
                fc_entry_free(e);
            } else {
                e->pending_reclaim = true;
            }
            if (osc) {
                fc_oscillate_t *dst_osc = (fc_oscillate_t *)art_search(&dst->oscillate_tree, e->key, e->key_len);
                if (dst_osc) {
                    dst_osc->freq += osc->freq;
                    dst_osc->io_len += osc->io_len;
                    dst_osc->osc_count += osc->osc_count;
                    dst->total_oscillations += osc->osc_count;
                    kfree(osc);
                } else {
                    art_insert(&dst->oscillate_tree, e->key, e->key_len, (void *)osc);
                    dst->total_oscillations += osc->osc_count;
                }
            }
            continue;
        }

        void *art_val = art_delete(&src->index, e->key, e->key_len);
        if (!art_val) {
            e->pending_reclaim = true;
            if (osc) {
                art_insert(&src->oscillate_tree, e->key, e->key_len, (void *)osc);
                src->total_oscillations += osc->osc_count;
            }
            continue;
        }
        fc_lru_remove(src, e);
        src->total_cache_bytes -= e->data_len;
        src->total_cache_io    -= e->total_io_len;
        src->total_cache_freq  -= e->access_freq;
        src->total_entries--;
        src->migrations_out++;

        e->cpu_id = best_dst;
        art_insert(&dst->index, e->key, e->key_len, (void *)e);
        fc_lru_push_back(dst, e);
        
        dst->total_entries++;
        dst->total_cache_bytes += e->data_len;
        dst->total_cache_io += e->total_io_len;
        dst->total_cache_freq += e->access_freq;
        if (e->file_size > dst->max_file_size) dst->max_file_size = e->file_size;
        dst->migrations_in++;

        if (osc) {
            art_insert(&dst->oscillate_tree, e->key, e->key_len, (void *)osc);
            dst->total_oscillations += osc->osc_count;
        }

        migrated++;
    }

    spinlock_unlock(&src->lock);
    spinlock_unlock(&dst->lock);
}

/* ===================== CPU Idle Handler & Writeback ===================== */
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
    if (!s) return;
    
    // 均值更新
    spinlock_lock(&s->lock);
    fc_update_averages_internal(s);
    spinlock_unlock(&s->lock);

    // Phase 1: 回收孤儿节点
    spinlock_lock(&s->lock);
    file_cache_entry_t *cur = s->lru_head;
    while (cur && s->total_entries > 0) {
        file_cache_entry_t *next = cur->lru_next;
        if (cur->pending_reclaim && cur->pin_count == 0) {
            fc_lru_remove(s, cur);
            s->total_cache_bytes -= cur->data_len;
            if (cur->is_dirty) s->dirty_cache_bytes -= cur->data_len;
            s->total_cache_io    -= cur->total_io_len;
            s->total_cache_freq  -= cur->access_freq;
            s->total_entries--;
            fc_entry_free(cur);
        }
        cur = next;
    }
    spinlock_unlock(&s->lock);

    // Phase 2: 刷脏页
    file_cache_entry_t *flush_list[FC_IDLE_FLUSH_BATCH];
    uint32_t flush_cnt = 0;

    spinlock_lock(&s->lock);
    cur = s->lru_head;
    while (cur && flush_cnt < FC_IDLE_FLUSH_BATCH) {
        if (cur->is_dirty && cur->pin_count == 0) {
            void *art_val = art_delete(&s->index, cur->key, cur->key_len);
            if (art_val) {
                fc_lru_remove(s, cur);
                s->total_cache_bytes -= cur->data_len;
                s->dirty_cache_bytes -= cur->data_len;
                s->total_cache_io    -= cur->total_io_len;
                s->total_cache_freq  -= cur->access_freq;
                s->total_entries--;
                flush_list[flush_cnt++] = cur;
            } else {
                cur->pending_reclaim = true;
            }
        }
        cur = cur->lru_next;
    }
    spinlock_unlock(&s->lock);

    if (s->writeback_cb) {
        for (uint32_t i = 0; i < flush_cnt; i++) {
            s->writeback_cb(flush_list[i]->key, flush_list[i]->key_len, flush_list[i]->data, flush_list[i]->data_len);
        }
    }

    // 插回 LRU 尾部
    spinlock_lock(&s->lock);
    for (uint32_t i = 0; i < flush_cnt; i++) {
        file_cache_entry_t *e = flush_list[i];
        e->is_dirty = false;
        
        file_cache_entry_t *exist = (file_cache_entry_t *)art_search(&s->index, e->key, e->key_len);
        if (exist) {
            fc_entry_free(e);
        } else {
            art_insert(&s->index, e->key, e->key_len, (void *)e);
            fc_lru_push_back(s, e);
            s->total_entries++;
            s->total_cache_bytes += e->data_len;
            s->total_cache_io += e->total_io_len;
            s->total_cache_freq += e->access_freq;
        }
    }
    spinlock_unlock(&s->lock);

    // Phase 3.5: 配额裁剪 (拆分持锁区间)
    spinlock_lock(&s->lock);
    if (s->max_file_size > 0 && s->base_memory > 0) {
        art_tree file_stats_tree;
        art_tree_init(&file_stats_tree);
        
        file_cache_entry_t *e_quota = s->lru_tail;
        uint32_t quota_scan_cnt = 0;
        while (e_quota && quota_scan_cnt < FC_QUOTA_SCAN_BATCH) {
            file_cache_entry_t *prev = e_quota->lru_prev;
            if (e_quota->file_id != 0 && e_quota->pin_count == 0 && e_quota->state == FC_STATE_CACHED && !e_quota->is_dirty) {
                uint64_t fid = e_quota->file_id;
                fc_file_stat_t *stat = (fc_file_stat_t *)art_search(&file_stats_tree, (const uint8_t*)&fid, sizeof(fid));
                if (!stat) {
                    stat = (fc_file_stat_t *)kmalloc(sizeof(fc_file_stat_t));
                    if (!stat) { e_quota = prev; quota_scan_cnt++; continue; }
                    stat->total_cached = 0;
                    stat->quota = (s->base_memory * e_quota->file_size) / s->max_file_size;
                    art_insert(&file_stats_tree, (const uint8_t*)&fid, sizeof(fid), (void*)stat);
                }
                
                if (stat->total_cached + e_quota->data_len > stat->quota) {
                    bool is_oscillating = (e_quota->osc_count > 0 && s->avg_osc_cache > 0 && e_quota->osc_count > s->avg_osc_cache);
                    if (is_oscillating) {
                        stat->total_cached += e_quota->data_len;
                    } else {
                        void *art_val = art_delete(&s->index, e_quota->key, e_quota->key_len);
                        if (art_val) {
                            fc_lru_remove(s, e_quota);
                            s->total_cache_bytes -= e_quota->data_len;
                            s->total_cache_io -= e_quota->total_io_len;
                            s->total_cache_freq -= e_quota->access_freq;
                            s->total_entries--;
                            s->evictions++;
                            
                            fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, e_quota->key, e_quota->key_len);
                            if (!osc) {
                                osc = (fc_oscillate_t *)kmalloc(sizeof(fc_oscillate_t));
                                if (osc) {
                                    osc->freq = e_quota->access_freq;
                                    osc->io_len = e_quota->total_io_len;
                                    osc->osc_count = e_quota->osc_count + 1;
                                    if (osc->osc_count > FC_OSC_COUNT_LIMIT) osc->osc_count = FC_OSC_COUNT_LIMIT;
                                    osc->file_size = e_quota->file_size;
                                    osc->file_id = e_quota->file_id;
                                    art_insert(&s->oscillate_tree, e_quota->key, e_quota->key_len, (void *)osc);
                                    s->total_oscillations += osc->osc_count;
                                }
                            } else {
                                osc->freq += e_quota->access_freq;
                                osc->io_len += e_quota->total_io_len;
                                s->total_oscillations -= osc->osc_count; 
                                osc->osc_count += (e_quota->osc_count + 1);
                                if (osc->osc_count > FC_OSC_COUNT_LIMIT) osc->osc_count = FC_OSC_COUNT_LIMIT;
                                s->total_oscillations += osc->osc_count;
                                if (e_quota->file_size > 0) osc->file_size = e_quota->file_size;
                                osc->file_id = e_quota->file_id;
                            }
                            
                            fc_entry_free(e_quota);
                        } else {
                            e_quota->pending_reclaim = true;
                        }
                    }
                } else {
                    stat->total_cached += e_quota->data_len;
                }
            }
            e_quota = prev;
            quota_scan_cnt++;
        }
        
        art_iter(&file_stats_tree, fc_free_file_stat_cb, NULL);
        art_tree_destroy(&file_stats_tree);
    }
    spinlock_unlock(&s->lock);

    // Phase 3: 动态由最冷端向最热端判断
    spinlock_lock(&s->lock);
    cur = s->lru_head;
    int32_t scan_cnt = 0;
    uint64_t batch_max_size = 0; 
    while (cur && scan_cnt < FC_IDLE_REVERSE_SCAN) {
        file_cache_entry_t *next = cur->lru_next;
        if (cur->file_size > batch_max_size) batch_max_size = cur->file_size;
        if (cur->pin_count == 0 && cur->state == FC_STATE_CACHED && !cur->is_dirty) {
            if (file_cache_should_evict(s, cur)) {
                void *art_val = art_delete(&s->index, cur->key, cur->key_len);
                if (art_val) {
                    fc_lru_remove(s, cur);
                    s->total_cache_bytes -= cur->data_len;
                    s->total_cache_io    -= cur->total_io_len;
                    s->total_cache_freq  -= cur->access_freq;
                    s->total_entries--;
                    s->evictions++;

                    fc_oscillate_t *osc = (fc_oscillate_t *)art_search(&s->oscillate_tree, cur->key, cur->key_len);
                    if (!osc) {
                        osc = (fc_oscillate_t *)kmalloc(sizeof(fc_oscillate_t));
                        if (osc) {
                            osc->freq = cur->access_freq;
                            osc->io_len = cur->total_io_len;
                            osc->osc_count = cur->osc_count + 1;
                            if (osc->osc_count > FC_OSC_COUNT_LIMIT) osc->osc_count = FC_OSC_COUNT_LIMIT;
                            osc->file_size = cur->file_size;
                            osc->file_id = cur->file_id;
                            art_insert(&s->oscillate_tree, cur->key, cur->key_len, (void *)osc);
                            s->total_oscillations += osc->osc_count;
                        }
                    } else {
                        osc->freq += cur->access_freq;
                        osc->io_len += cur->total_io_len;
                        s->total_oscillations -= osc->osc_count; 
                        osc->osc_count += (cur->osc_count + 1);
                        if (osc->osc_count > FC_OSC_COUNT_LIMIT) osc->osc_count = FC_OSC_COUNT_LIMIT;
                        s->total_oscillations += osc->osc_count;
                        if (cur->file_size > 0) osc->file_size = cur->file_size;
                        osc->file_id = cur->file_id;
                    }

                    fc_entry_free(cur);
                } else {
                    cur->pending_reclaim = true;
                }
            }
        }
        cur = next;
        scan_cnt++;
    }

    if (batch_max_size > 0) {
        s->max_file_size = (s->max_file_size * 7 + batch_max_size) / 8;
    } else if (s->lru_head == NULL) {
        s->max_file_size = 0; 
    }
    spinlock_unlock(&s->lock);

    // Phase 4: 强制驱逐
    spinlock_lock(&s->lock);
    if (s->total_cache_bytes > (s->base_memory * 90) / 100) {
        int32_t evicted = 0;
        while (evicted < FC_IDLE_FLUSH_BATCH && s->total_entries > 0) {
            if (fc_evict_one(s) == 0) evicted++;
            else break;
        }
    }
    spinlock_unlock(&s->lock);
}

/* ===================== Periodic Tick & Oscillate Decay ===================== */
typedef struct {
    file_cache_cpu_t *s;
    uint32_t batch;
    const uint8_t* del_keys[FC_DECAY_OSC_BATCH];
    uint32_t del_lens[FC_DECAY_OSC_BATCH];
    uint32_t del_cnt;
} fc_osc_decay_ctx;

static int fc_osc_decay_cb(void *data, const uint8_t *key, uint32_t key_len, void *value) {
    fc_osc_decay_ctx *ctx = (fc_osc_decay_ctx *)data;
    if (ctx->batch >= FC_DECAY_OSC_BATCH) return 1; 
    
    fc_oscillate_t *osc = (fc_oscillate_t *)value;
    uint64_t old_osc = osc->osc_count;
    osc->osc_count >>= 1;
    ctx->s->total_oscillations -= (old_osc - osc->osc_count);
    
    osc->freq >>= 1;
    osc->io_len >>= 1;
    
    if (osc->osc_count == 0 && osc->freq == 0) {
        if (ctx->del_cnt < FC_DECAY_OSC_BATCH) {
            ctx->del_keys[ctx->del_cnt] = key;
            ctx->del_lens[ctx->del_cnt] = key_len;
            ctx->del_cnt++;
        }
    }
    ctx->batch++;
    return 0;
}

void file_cache_tick(file_cache_cpu_t *s) {
    if (!s) return;
    
    spinlock_lock(&s->lock);
    s->clock++;

    fc_update_averages_internal(s);

    bool do_decay = (s->clock - s->last_decay_tick >= FC_HISTORY_DECAY_TICKS);
    if (do_decay) {
        s->last_decay_tick = s->clock;
        uint32_t batch = 0;
        
        if (!s->decay_cursor) s->decay_cursor = s->lru_head;
        while (s->decay_cursor && batch < FC_DECAY_CACHE_BATCH) {
            file_cache_entry_t *e = s->decay_cursor;
            s->decay_cursor = e->lru_next;
            
            s->total_cache_io -= e->total_io_len;
            s->total_cache_freq -= e->access_freq;
            e->access_freq >>= 1;
            e->total_io_len >>= 1;
            s->total_cache_io += e->total_io_len;
            s->total_cache_freq += e->access_freq;
            batch++;
        }
        
        fc_osc_decay_ctx ctx = {s, 0, {0}, {0}, 0};
        art_iter(&s->oscillate_tree, fc_osc_decay_cb, &ctx);
        
        for (uint32_t i = 0; i < ctx.del_cnt; i++) {
            void *val = art_delete(&s->oscillate_tree, ctx.del_keys[i], ctx.del_lens[i]);
            if (val) kfree(val);
        }
    }
    spinlock_unlock(&s->lock);
}