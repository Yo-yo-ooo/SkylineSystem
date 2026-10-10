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

#pragma region Constants & Macros

#define FC_MAX_CPUS              128
#define FC_ERR_NO_MEMORY         -2
#define FC_ERR_FLUSHING          -3

#define FC_INLINE_DATA_SIZE      64
#define FC_TINY_FILE_THRESHOLD   4096
#define FC_FSYNC_BATCH_SIZE      256

#pragma endregion

#pragma region Data Structures

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
    uint32_t            crc32;          

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
    
    uint8_t             inline_data[0]; 
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
    uint64_t tiny_cache_bytes;      
    
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
    uint64_t readahead_evictions; 

    uint64_t clock;
    uint64_t last_decay_tick;

    uint32_t io_congestion;
    uint64_t total_writeback_failures;

    /* 审计 #5 (round 5): 回调签名补 file_id —— 写回必须经原始 filedesc
       直写 (消除路径重开 + O_TRUNC 截断同名新文件的风险) */
    int32_t (*writeback_cb)(uint64_t file_id, const uint8_t *key, uint32_t key_len, void *data, size_t data_len);
} file_cache_cpu_t;

#pragma endregion

#pragma region Public API

int32_t file_cache_fsync(file_cache_cpu_t *s, uint64_t file_id);

void    file_cache_cpu_init(file_cache_cpu_t *s, uint32_t cpu_id, 
                            int32_t (*writeback_cb)(uint64_t, const uint8_t*, uint32_t, void*, size_t));
void    file_cache_cpu_destroy(file_cache_cpu_t *s); 

void    file_cache_set_limits(file_cache_cpu_t *s, uint64_t soft_limit, uint64_t hard_limit);

void*   file_cache_get(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                       size_t io_len, size_t *out_len, file_cache_entry_t **out_entry);
void    file_cache_put(file_cache_cpu_t *s, file_cache_entry_t *e);

int32_t file_cache_record_io(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                             size_t io_len, void *data_if_promote, uint64_t file_size, uint64_t file_id);

int32_t file_cache_promote(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                           void *data, size_t data_len, bool is_dirty, uint64_t file_size, uint64_t file_id);

int32_t file_cache_readahead(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len,
                             void *data, size_t data_len, uint64_t file_size, uint64_t file_id);

int32_t file_cache_invalidate(file_cache_cpu_t *s, const uint8_t *key, uint32_t key_len);

/* ---- 块级缓存 API (fc-block-cache-design.md Step 1, round 26) ----
   键 = (file_id, block#) 16 字节; 与 path 版并存, 语义相同 (本阶段
   仍为整条目语义, 键构造先行 —— 后续 Step 2/3 由 fops 逐块调用后
   自然获得块粒度)

   ⚠ 入缓存的块数据必须**从块首开始**: 命中路径以 (块号) 取块、再按调用
   方的 `块内偏移` 直接索引 data[from .. from+len)。若把"块内偏移 2644 处
   读到的 54 字节"当成一个块 promote, 同一块内其它偏移的合法读就会命中并
   拿到错位数据 —— 表现为文件内容随机损坏。见 fops.cpp 的 promote 循环。 */
void*   file_cache_get_block(file_cache_cpu_t *s, uint64_t file_id, uint64_t block,
                             size_t io_len, size_t *out_len, file_cache_entry_t **out_entry);
int32_t file_cache_promote_block(file_cache_cpu_t *s, uint64_t file_id, uint64_t block,
                                 void *data, size_t data_len, bool is_dirty,
                                 uint64_t file_size);
void    file_cache_invalidate_block(file_cache_cpu_t *s, uint64_t file_id, uint64_t block);
void    file_cache_invalidate_file(file_cache_cpu_t *s, uint64_t file_id,
                                   uint64_t file_size);

/* 块缓存的 file_id 分配器。fops 用它替代 `FD->filedesc` 指针: 指针在
   fclose 之后会被下一次 open 的 kmalloc 复用, 于是两个不同文件共用同一批
   (file_id, block#) 键 —— 读新文件会静默返回旧文件的数据。 */
uint64_t fc_next_file_uid(void);

void    file_cache_check_load(file_cache_cpu_t *s, uint32_t load_factor);
void    file_cache_idle_handler(file_cache_cpu_t *s);
void    file_cache_tick(file_cache_cpu_t *s);

#pragma endregion

#ifdef __cplusplus
}
#endif

#endif /* _FILE_CACHE_H_ */