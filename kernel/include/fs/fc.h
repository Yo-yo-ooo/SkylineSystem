// SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#ifndef _FILE_CACHE_H_
#define _FILE_CACHE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <klib/algorithm/art.h>
#include <klib/klib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FC_MAX_CPUS              128
#define FC_ERR_NO_MEMORY         -2
#define FC_ERR_FLUSHING          -3  // 表示因正在刷脏而临时拒绝写入

typedef enum {
    FC_STATE_CACHED           = 0,
    FC_STATE_EVICTING         = 1,
    FC_STATE_INVALID          = 2,
    FC_STATE_WRITEBACK_FAILED = 3,
    FC_STATE_FLUSHING         = 4
} fc_state_t;

typedef struct fc_oscillate {
    uint64_t freq;          
    uint64_t io_len;        
    uint64_t osc_count;     
    uint64_t file_size;     
    uint64_t file_id;       
} fc_oscillate_t;

typedef struct file_cache_entry {
    uint8_t            *key;
    uint32_t            key_len;
    void               *data;
    size_t              data_len;

    uint64_t            access_freq;
    uint64_t            total_io_len;
    uint64_t            last_access_tick;
    uint64_t            create_tick;
    uint64_t            file_size;     
    uint64_t            osc_count;     
    uint64_t            file_id;       

    fc_state_t          state;
    uint32_t            pin_count;
    uint32_t            cpu_id;       
    bool                is_dirty;     
    bool                pending_reclaim;
    uint8_t             writeback_retries;

    struct file_cache_entry *lru_prev;
    struct file_cache_entry *lru_next;
} file_cache_entry_t;

typedef struct file_cache_cpu {
    art_tree             index;          
    art_tree             oscillate_tree; 
    
    spinlock_t           lock;
    uint32_t             cpu_id;         

    file_cache_entry_t  *lru_head;
    file_cache_entry_t  *lru_tail;
    file_cache_entry_t  *decay_cursor;      

    uint64_t total_cache_io;        
    uint64_t total_cache_freq;      
    uint64_t total_oscillations;    
    uint64_t max_file_size;         
    
    uint64_t total_cache_bytes;   
    uint64_t dirty_cache_bytes;
    uint64_t smoothed_cache_bytes; 
    
    uint64_t soft_limit;
    uint64_t hard_limit;
    
    uint64_t avg_io_cache;
    uint64_t avg_freq_cache;
    uint64_t avg_osc_cache;
    
    uint32_t total_entries;       
    uint32_t evict_scan_window;
    int32_t  evict_hit_count;
    uint32_t evict_miss_count;
    uint32_t evict_hit_threshold;
    
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t migrations_in;
    uint64_t migrations_out;

    uint64_t clock;
    uint64_t last_decay_tick;

    uint32_t io_congestion;
    uint64_t total_writeback_failures;

    int32_t (*writeback_cb)(const uint8_t *key, uint32_t key_len, void *data, size_t data_len);
} file_cache_cpu_t;

int32_t file_cache_fsync(file_cache_cpu_t *s, uint64_t file_id);

void    file_cache_cpu_init(file_cache_cpu_t *s, uint32_t cpu_id, 
                            int32_t (*writeback_cb)(const uint8_t*, uint32_t, void*, size_t));
void    file_cache_cpu_destroy(file_cache_cpu_t *s); // 用于销毁分片并释放对象池

void    file_cache_set_limits(file_cache_cpu_t *s, uint64_t soft_limit, uint64_t hard_limit);

void*   file_cache_get(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                       size_t io_len, size_t *out_len, file_cache_entry_t **out_entry);
void    file_cache_put(file_cache_cpu_t *s, file_cache_entry_t *e);

int32_t file_cache_record_io(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                             size_t io_len, void *data_if_promote, uint64_t file_size, uint64_t file_id);

int32_t file_cache_promote(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                           void *data, size_t data_len, bool is_dirty, uint64_t file_size, uint64_t file_id);

int32_t file_cache_invalidate(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len);

void    file_cache_check_load(file_cache_cpu_t *s, uint32_t load_factor);
void    file_cache_idle_handler(file_cache_cpu_t *s);
void    file_cache_tick(file_cache_cpu_t *s);

#ifdef __cplusplus
}
#endif

#endif /* _FILE_CACHE_H_ */