/*
* SPDX-License-Identifier: GPL-2.0-only
* File: fd.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <fs/fd.h>
#include <fs/lwext4/ext4.h>
#include <klib/algorithm/hmap.h>



#include <klib/klib.h>
#include <mem/heap.h>

extern "C" char* GetMountPointName(const char* path) {
    if (!path || path[0] != '/') return nullptr;

    // 找到第一个有效字符后的第一个 '/'
    // 比如 "/mp/test" -> 找到 'p' 后面的那个 '/'
    const char* p = path + 1;
    while (*p && *p == '/') p++; // 跳过开头重复的斜杠，如 "//mp" -> 指向 "mp"
    
    const char* second_slash = strchr(p, '/');
    
    // 如果是 "/mp" 这种没有结尾斜杠的，根据的需求决定是否支持
    // 如果必须有第二个斜杠才叫挂载点：
    if (!second_slash) return nullptr;

    size_t len = (size_t)(second_slash - path) + 1;

    char* mp_name = (char*)kmalloc(len + 1);
    if (!mp_name) return nullptr; 

    __memcpy(mp_name, path, len);
    mp_name[len] = '\0';

    // 可以在这里做一个标准化：将 "///mp/" 变成 "/mp/"
    // 或者在挂载查找逻辑里处理多斜杠情况
    //kinfoln("Extracted mount point name: %s", mp_name);
    return mp_name;
}




extern "C" int32_t __hmap_s_mp_compare(const void* a,const void* b,void *udata){
    return strcmp(((struct __hmap_s_mp*)a)->MPName,((struct __hmap_s_mp*)b)->MPName);
}



extern "C" uint64_t __hmap_s_mp_hash(const void* item, uint64_t seed0, uint64_t seed1){
    const struct __hmap_s_mp* entry = (const struct __hmap_s_mp*)item;
    return hashmap_sip(entry->MPName, strlen(entry->MPName), seed0, seed1);
}



extern "C" struct hashmap* HMapS_MP = nullptr;

void InitFFMAN(){
    HMapS_MP = hashmap_new(sizeof(struct __hmap_s_mp), 16,0,0,
        __hmap_s_mp_hash, __hmap_s_mp_compare, nullptr, nullptr);
}
spinlock_t hmap_mp_lock;
static __hmap_s_mp *GetMount_NoLock(const char *path)
{
    if (!HMapS_MP || !path)
        return nullptr;

    char* mp_name = GetMountPointName(path);
    if (!mp_name)
        return nullptr;

    struct __hmap_s_mp temp_key = { .MPName = mp_name };
    __hmap_s_mp *hsmp = (__hmap_s_mp*)hashmap_get(HMapS_MP, &temp_key);

    // 立刻释放提取的挂载名字符串，杜绝内存泄漏
    kfree(mp_name);
    return hsmp;
}


__hmap_s_mp *GetMount(const char *path)
{
    spinlock_lock(&hmap_mp_lock);
    __hmap_s_mp *ret = GetMount_NoLock(path);
    spinlock_unlock(&hmap_mp_lock);
    return ret;
}
extern "C" {

void fd_manager_init(fd_manager_t* manager) {
    manager->head = nullptr;
    manager->max_fd = -1; // 初始状态为 -1，表示没有分配过 FD
}

static fd_node_t* create_fd_node() {
    fd_node_t* node = (fd_node_t*)kmalloc(sizeof(fd_node_t)); 
    if (!node) return nullptr;
    
    _memset(node->bitmap, 0, sizeof(node->bitmap));
    _memset(node->fds, 0, sizeof(node->fds));
    node->alloc_count = 0;
    node->next = nullptr;
    
    return node;
}

int32_t fd_alloc(fd_manager_t* manager, fd_t** out_fd_ptr) {
    spinlock_lock(&manager->lock); // 加锁

    fd_node_t* curr = manager->head;
    fd_node_t* prev = nullptr;
    uint32_t node_index = 0;

    if (!curr) {
        manager->head = create_fd_node();
        curr = manager->head;
        if (!curr) {
            spinlock_unlock(&manager->lock);
            return -1;
        }
    }

    while (curr) {
        if (curr->alloc_count < FDS_PER_NODE) {
            for (int i = 0; i < 4; i++) {
                if (curr->bitmap[i] != ~(0ULL)) {
                    uint64_t inverted = ~(curr->bitmap[i]);
                    int bit_idx = __builtin_ctzll(inverted);
                    
                    curr->bitmap[i] |= (1ULL << bit_idx);
                    curr->alloc_count++;
                    
                    int32_t global_fd = (node_index * FDS_PER_NODE) + (i * 64) + bit_idx;
                    
                    if (global_fd > manager->max_fd) {
                        manager->max_fd = global_fd;
                    }
                    
                    if (out_fd_ptr) {
                        *out_fd_ptr = &curr->fds[(i * 64) + bit_idx];
                        (*out_fd_ptr)->filedesc = nullptr;
                        (*out_fd_ptr)->FSOPS = nullptr;
                        (*out_fd_ptr)->MP = nullptr;
                    }
                    
                    spinlock_unlock(&manager->lock); // 解锁
                    return global_fd;
                }
            }
        }
        node_index++;
        prev = curr;
        curr = curr->next;
    }

    fd_node_t* new_node = create_fd_node();
    if (!new_node) {
        spinlock_unlock(&manager->lock);
        return -1;
    }
    
    prev->next = new_node;
    
    new_node->bitmap[0] |= 1ULL;
    new_node->alloc_count = 1;
    
    int32_t global_fd = node_index * FDS_PER_NODE;
    
    if (global_fd > manager->max_fd) {
        manager->max_fd = global_fd;
    }
    
    if (out_fd_ptr) {
        *out_fd_ptr = &new_node->fds[0];
        (*out_fd_ptr)->filedesc = nullptr;
        (*out_fd_ptr)->FSOPS = nullptr;
        (*out_fd_ptr)->MP = nullptr;
    }
    
    spinlock_unlock(&manager->lock); // 解锁
    return global_fd;
}

fd_t* fd_get(fd_manager_t* manager, int32_t fd) {
    if (fd < 0 || fd > manager->max_fd) return nullptr;

    uint32_t target_node_index = fd / FDS_PER_NODE;
    uint32_t local_idx = fd % FDS_PER_NODE;
    
    fd_node_t* curr = manager->head;
    uint32_t curr_index = 0;
    
    while (curr && curr_index < target_node_index) {
        curr = curr->next;
        curr_index++;
    }

    if (!curr) return nullptr;

    uint32_t array_idx = local_idx / 64;
    uint32_t bit_idx   = local_idx % 64;

    if (!(curr->bitmap[array_idx] & (1ULL << bit_idx))) {
        return nullptr;
    }

    return &curr->fds[local_idx];
}

void fd_free(fd_manager_t* manager, int32_t fd) {
    if (fd < 0) return;

    spinlock_lock(&manager->lock); // 加锁

    // 快速拦截：如果比当前记录的最大值还大，肯定非法
    if (fd > manager->max_fd) {
        spinlock_unlock(&manager->lock);
        return;
    }

    uint32_t target_node_index = fd / FDS_PER_NODE;
    uint32_t local_idx = fd % FDS_PER_NODE;
    
    fd_node_t* curr = manager->head;
    uint32_t curr_index = 0;
    
    while (curr && curr_index < target_node_index) {
        curr = curr->next;
        curr_index++;
    }

    if (!curr) {
        spinlock_unlock(&manager->lock);
        return;
    }

    uint32_t array_idx = local_idx / 64;
    uint32_t bit_idx   = local_idx % 64;

    if (curr->bitmap[array_idx] & (1ULL << bit_idx)) {
        curr->bitmap[array_idx] &= ~(1ULL << bit_idx);
        curr->alloc_count--;
        
        curr->fds[local_idx].filedesc = nullptr;
        curr->fds[local_idx].FSOPS = nullptr;
        curr->fds[local_idx].MP = nullptr;
    }
    if (fd == manager->max_fd) {
        manager->max_fd--;
    }

    spinlock_unlock(&manager->lock); // 解锁
}

void fd_manager_destroy(fd_manager_t* manager) {
    if (!manager || !manager->head) return;

    fd_node_t* curr = manager->head;
    while (curr) {
        fd_node_t* next = curr->next;
        
        if (curr->alloc_count > 0) {
            for (int i = 0; i < 4; i++) {
                uint64_t bm = curr->bitmap[i];
                while (bm) {
                    int bit_idx = __builtin_ctzll(bm);
                    uint32_t local_idx = (i * 64) + bit_idx;
                    fd_t* fd_ptr = &curr->fds[local_idx];
                    
                    if (fd_ptr->FSOPS && fd_ptr->FSOPS->close) {
                        fd_ptr->FSOPS->close(fd_ptr->filedesc);
                    }
                    
                    fd_ptr->filedesc = nullptr;
                    fd_ptr->FSOPS = nullptr;
                    fd_ptr->MP = nullptr;
                    
                    // 清除最低位的 1，进入下一个已分配的 FD
                    bm &= bm - 1; 
                }
            }
        }
        
        kfree(curr);
        curr = next;
    }

    manager->head = nullptr;
    manager->max_fd = -1;
}

} // extern "C"