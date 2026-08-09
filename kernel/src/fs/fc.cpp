// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#include <fs/fc.h>
#include <mem/heap.h>
#include <klib/algorithm/art.h>

extern "C" void *__memcpy(void *d, const void *s, uint64_t n);
extern void  spinlock_lock(spinlock_t* lock);
extern void  spinlock_unlock(spinlock_t* lock);

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
    
    if (s->smoothed_cache_bytes > 0) {
        s->avg_freq_cache = s->total_cache_freq / s->smoothed_cache_bytes;
    } else {
        s->avg_freq_cache = 0;
    }

    if (s->oscillate_tree.size > 0) {
        s->avg_osc_cache = s->total_oscillations / s->oscillate_tree.size;
    } else {
        s->avg_osc_cache = 0;
    }

    s->smoothed_cache_bytes = (s->smoothed_cache_bytes * 7 + s->total_cache_bytes) / 8;
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

static file_cache_entry_t *fc_pick_and_unlink_victim(file_cache_cpu_t *s) {
    file_cache_entry_t *clean_fallback = NULL;
    int32_t scan_cnt = 0;
    bool hit = false;
    
    for (file_cache_entry_t *cur = s->lru_head; cur && scan_cnt < s->evict_scan_window; cur = cur->lru_next) {
        if (cur->pending_reclaim && cur->pin_count == 0) {
            fc_lru_remove(s, cur);
            s->total_cache_bytes -= cur->data_len;
            s->total_cache_io    -= cur->total_io_len;
            s->total_cache_freq  -= cur->access_freq;
            s->total_entries--;
            s->evictions++;
            return cur;
        }
        
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
                void *art_val = art_delete(&s->index, cur->key, cur->key_len);
                if (art_val) {
                    fc_lru_remove(s, cur);
                    s->total_cache_bytes -= cur->data_len;
                    s->total_cache_io    -= cur->total_io_len;
                    s->total_cache_freq  -= cur->access_freq;
                    s->total_entries--;
                    s->evictions++;
                    return cur;
                } else {
                    cur->pending_reclaim = true;
                }
            }
            scan_cnt++;
        }
    }
    
    if (!hit && clean_fallback) {
        s->evict_hit_count = 0;
        s->evict_miss_count++;
        if (s->evict_miss_count >= 8) s->evict_hit_threshold = FC_EVICT_HIT_THRESHOLD * 2;
        if (s->evict_scan_window < FC_EVICT_WINDOW_MAX) {
            s->evict_scan_window = s->evict_scan_window * 5 / 4;
            if (s->evict_scan_window > FC_EVICT_WINDOW_MAX) s->evict_scan_window = FC_EVICT_WINDOW_MAX;
        }
        void *art_val = art_delete(&s->index, clean_fallback->key, clean_fallback->key_len);
        if (art_val) {
            fc_lru_remove(s, clean_fallback);
            s->total_cache_bytes -= clean_fallback->data_len;
            s->total_cache_io    -= clean_fallback->total_io_len;
            s->total_cache_freq  -= clean_fallback->access_freq;
            s->total_entries--;
            s->evictions++;
            return clean_fallback;
        }
    }
    return NULL; 
}

static void fc_entry_free(file_cache_entry_t *e) {
    if (!e) return;
    if (e->data) kfree(e->data);
    if (e->key)  kfree(e->key);
    kfree(e);
}

static void fc_update_oscillate(file_cache_cpu_t *s, file_cache_entry_t *v) {
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
}

/* ===================== Init ===================== */
void file_cache_cpu_init(file_cache_cpu_t *s, uint32_t cpu_id, 
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
    s->smoothed_cache_bytes = 0;
    s->soft_limit = 0; s->hard_limit = 0; 
    s->avg_io_cache = 0; s->avg_freq_cache = 0; s->avg_osc_cache = 0;
    s->total_entries = 0;
    s->evict_scan_window = 64;
    s->evict_hit_count = 0; s->evict_miss_count = 0; s->evict_hit_threshold = FC_EVICT_HIT_THRESHOLD;
    s->hits = 0; s->misses = 0; s->evictions = 0;
    s->migrations_in = 0; s->migrations_out = 0;
    s->writeback_cb = writeback_cb;

    spinlock_lock(&g_fc_init_lock);
    if (cpu_id < FC_MAX_CPUS) {
        g_fc_cpus[cpu_id] = s;
        if (cpu_id + 1 > g_num_active_cpus) g_num_active_cpus = cpu_id + 1;
    }
    spinlock_unlock(&g_fc_init_lock);
}

void file_cache_set_limits(file_cache_cpu_t *s, uint64_t soft_limit, uint64_t hard_limit) {
    if (!s) return;
    spinlock_lock(&s->lock);
    s->soft_limit = soft_limit;
    s->hard_limit = hard_limit;
    spinlock_unlock(&s->lock);
}

/* ===================== Broadcast Invalidation ===================== */
static void fc_broadcast_invalidate(file_cache_cpu_t *src_s, const uint8_t *key, uint32_t key_len) {
    for (uint32_t i = 0; i < g_num_active_cpus; i++) {
        if (i == src_s->cpu_id) continue;
        file_cache_cpu_t *s = g_fc_cpus[i];
        if (!s) continue;
        
        file_cache_entry_t *entry_to_free = NULL;
        fc_oscillate_t *osc_to_free = NULL;

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
                    entry_to_free = e;
                } else {
                    e->pending_reclaim = true;
                }
            }
        }
        osc_to_free = (fc_oscillate_t *)art_delete(&s->oscillate_tree, key, key_len);
        if (osc_to_free) {
            s->total_oscillations -= osc_to_free->osc_count;
        }
        spinlock_unlock(&s->lock);

        if (entry_to_free) fc_entry_free(entry_to_free);
        if (osc_to_free) kfree(osc_to_free);
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
            if (s->soft_limit > 0 && s->smoothed_cache_bytes + re->data_len > s->soft_limit) {
                spinlock_unlock(&rs->lock);
                continue; 
            }

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
    bool need_free = false;
    if (e->pin_count > 0) e->pin_count--;

    if (e->pin_count == 0 && (e->state == FC_STATE_INVALID || e->pending_reclaim)) {
        if (!e->pending_reclaim) {
            void *art_val = art_delete(&s->index, e->key, e->key_len);
            if (!art_val) {
                e->pending_reclaim = true;
            } else {
                fc_lru_remove(s, e);
                s->total_cache_bytes -= e->data_len;
                if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
                s->total_cache_io    -= e->total_io_len;
                s->total_cache_freq  -= e->access_freq;
                s->total_entries--;
                need_free = true;
            }
        } else {
            fc_lru_remove(s, e);
            s->total_cache_bytes -= e->data_len;
            if (e->is_dirty) s->dirty_cache_bytes -= e->data_len;
            s->total_cache_io    -= e->total_io_len;
            s->total_cache_freq  -= e->access_freq;
            s->total_entries--;
            need_free = true;
        }
    }
    spinlock_unlock(&s->lock);
    if (need_free) fc_entry_free(e); 
}

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
    uint64_t cur_osc  = osc ? osc->osc_count : 0;
    uint64_t cur_fsize = osc ? osc->file_size : file_size;

    if (cur_fsize > 0 && cur_fsize > s->max_file_size) {
        s->max_file_size = cur_fsize;
    }
    spinlock_unlock(&s->lock);

    bool should = file_cache_should_cache(s, cur_freq, io_len, cur_osc);
    if (should && data_if_promote) {
        int32_t r = file_cache_promote(s, key, key_len, data_if_promote, io_len, false, cur_fsize, file_id);
        if (r != 0) {
            spinlock_lock(&s->lock);
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
            }
            spinlock_unlock(&s->lock);
            kfree(data_if_promote);
            return r;
        }
        return (int32_t)io_len;
    } else {
        spinlock_lock(&s->lock);
        if (!osc) {
            osc = (fc_oscillate_t *)kmalloc(sizeof(fc_oscillate_t));
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
        if (v) {
            fc_update_oscillate(s, v);
            victims[vic_cnt++] = v;
        } else {
            break;
        }
    }
    return vic_cnt;
}

int32_t file_cache_promote(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                           void *data, size_t data_len, bool is_dirty, uint64_t file_size, uint64_t file_id) {
    if (!s || !key || key_len == 0 || !data || data_len == 0) return -1;

    file_cache_entry_t *e = (file_cache_entry_t *)kcalloc(1, sizeof(file_cache_entry_t));
    if (!e) return FC_ERR_NO_MEMORY;

    e->key = (uint8_t *)kmalloc(key_len);
    if (!e->key) {
        kfree(e);
        return FC_ERR_NO_MEMORY;
    }
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

    bool need_broadcast = false;
    file_cache_entry_t *victims[8] = {0};
    int vic_cnt = 0;
    void *old_data_to_free = NULL;

    spinlock_lock(&s->lock);
    
    if (file_size > 0 && file_size > s->max_file_size) {
        s->max_file_size = file_size;
    }

    file_cache_entry_t *exist = (file_cache_entry_t *)art_search(&s->index, key, key_len);
    if (exist) {
        old_data_to_free = exist->data; 
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
        
        if (is_dirty) need_broadcast = true;
        spinlock_unlock(&s->lock);
        
        if (old_data_to_free) kfree(old_data_to_free);
        kfree(e->key); kfree(e); 
        if (need_broadcast) fc_broadcast_invalidate(s, key, key_len);
        return 0;
    }

    if (is_dirty && s->total_cache_bytes > 0 && 
        (s->dirty_cache_bytes * 100 / s->total_cache_bytes) > FC_DIRTY_RATIO_LIMIT) {
        spinlock_unlock(&s->lock);
        kfree(e->key); kfree(e);
        return FC_ERR_NO_MEMORY;
    }

    if (s->hard_limit > 0 && s->total_cache_bytes + data_len > s->hard_limit) {
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
    } else {
        e->access_freq = 1;
        e->osc_count = 0;
    }

    art_insert(&s->index, e->key, e->key_len, (void *)e);
    if (art_search(&s->index, e->key, e->key_len) != e) {
        spinlock_unlock(&s->lock);
        kfree(e->key); kfree(e);
        for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
        return -4;
    }

    fc_lru_push_back(s, e);
    s->total_entries++;
    s->total_cache_bytes += data_len;
    if (is_dirty) s->dirty_cache_bytes += data_len;
    s->total_cache_io    += e->total_io_len;
    s->total_cache_freq  += e->access_freq;

    if (is_dirty) need_broadcast = true;
    spinlock_unlock(&s->lock);

    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
    if (need_broadcast) fc_broadcast_invalidate(s, key, key_len);
    
    return 0;
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
    uint64_t lowest_load = 100;
    
    uint64_t src_load = (src->soft_limit > 0) ? (src->smoothed_cache_bytes * 100 / src->soft_limit) : 0;
    if (src_load < 80) return;

    for (uint32_t i = 0; i < g_num_active_cpus; i++) {
        if (i == src->cpu_id || !g_fc_cpus[i]) continue;
        file_cache_cpu_t *dst = g_fc_cpus[i];
        
        uint64_t dst_load = (dst->soft_limit > 0) ? (dst->smoothed_cache_bytes * 100 / dst->soft_limit) : 0;
        if (dst_load < lowest_load) {
            lowest_load = dst_load;
            best_dst = i;
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

    file_cache_entry_t *victims[FC_MIGRATE_BATCH_SIZE] = {0};
    int vic_cnt = 0;

    int32_t migrated = 0;
    while (migrated < FC_MIGRATE_BATCH_SIZE && src->lru_head) {
        file_cache_entry_t *e = src->lru_head;
        if (e->pin_count > 0 || e->state != FC_STATE_CACHED || e->is_dirty) {
            fc_lru_move_to_back(src, e); 
            continue;
        }

        if (dst->soft_limit > 0 && dst->smoothed_cache_bytes + e->data_len > dst->soft_limit) {
            break;
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
                victims[vic_cnt++] = e;
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

    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
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
    
    spinlock_lock(&s->lock);
    fc_update_averages_internal(s);
    spinlock_unlock(&s->lock);

    file_cache_entry_t *victims[FC_IDLE_FLUSH_BATCH * 2] = {0};
    int vic_cnt = 0;

    // Phase 1: 回收孤儿节点
    spinlock_lock(&s->lock);
    file_cache_entry_t *cur = s->lru_head;
    while (cur && vic_cnt < FC_IDLE_FLUSH_BATCH) {
        file_cache_entry_t *next = cur->lru_next;
        if (cur->pending_reclaim && cur->pin_count == 0) {
            void *art_val = art_delete(&s->index, cur->key, cur->key_len);
            if (art_val) {
                fc_lru_remove(s, cur);
                s->total_cache_bytes -= cur->data_len;
                if (cur->is_dirty) s->dirty_cache_bytes -= cur->data_len;
                s->total_cache_io    -= cur->total_io_len;
                s->total_cache_freq  -= cur->access_freq;
                s->total_entries--;
                victims[vic_cnt++] = cur;
            }
        }
        cur = next;
    }
    spinlock_unlock(&s->lock);
    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
    vic_cnt = 0;

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

    // Phase 3.5: 配额裁剪
    spinlock_lock(&s->lock);
    if (s->max_file_size > 0 && s->smoothed_cache_bytes > 0) {
        art_tree file_stats_tree;
        art_tree_init(&file_stats_tree);
        
        file_cache_entry_t *e_quota = s->lru_tail;
        uint32_t quota_scan_cnt = 0;
        while (e_quota && quota_scan_cnt < FC_QUOTA_SCAN_BATCH && vic_cnt < FC_IDLE_FLUSH_BATCH) {
            file_cache_entry_t *prev = e_quota->lru_prev;
            if (e_quota->file_id != 0 && e_quota->pin_count == 0 && e_quota->state == FC_STATE_CACHED && !e_quota->is_dirty) {
                uint64_t fid = e_quota->file_id;
                fc_file_stat_t *stat = (fc_file_stat_t *)art_search(&file_stats_tree, (const uint8_t*)&fid, sizeof(fid));
                if (!stat) {
                    stat = (fc_file_stat_t *)kmalloc(sizeof(fc_file_stat_t));
                    if (!stat) { e_quota = prev; quota_scan_cnt++; continue; }
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
                        if (art_val) {
                            fc_lru_remove(s, e_quota);
                            s->total_cache_bytes -= e_quota->data_len;
                            s->total_cache_io -= e_quota->total_io_len;
                            s->total_cache_freq -= e_quota->access_freq;
                            s->total_entries--;
                            s->evictions++;
                            fc_update_oscillate(s, e_quota);
                            victims[vic_cnt++] = e_quota;
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
    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
    vic_cnt = 0;

    // Phase 3: 动态由最冷端向最热端判断
    spinlock_lock(&s->lock);
    cur = s->lru_head;
    int32_t scan_cnt = 0;
    uint64_t batch_max_size = 0; 
    while (cur && scan_cnt < FC_IDLE_REVERSE_SCAN && vic_cnt < FC_IDLE_FLUSH_BATCH) {
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
                    fc_update_oscillate(s, cur);
                    victims[vic_cnt++] = cur;
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

    // Phase 4: 硬水位强制驱逐
    if (s->hard_limit > 0 && s->total_cache_bytes > s->hard_limit) {
        while (s->total_cache_bytes > s->hard_limit && vic_cnt < FC_IDLE_FLUSH_BATCH) {
            file_cache_entry_t *v = fc_pick_and_unlink_victim(s);
            if (v) {
                fc_update_oscillate(s, v);
                victims[vic_cnt++] = v;
            } else {
                break;
            }
        }
    }
    spinlock_unlock(&s->lock);
    for (int i = 0; i < vic_cnt; i++) fc_entry_free(victims[i]);
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