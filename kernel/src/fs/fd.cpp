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

// 1. 初始化管理器
void fd_manager_init(fd_manager_t* manager) {
    manager->head = nullptr;
    manager->max_fd = -1; // 初始状态为 -1，表示没有分配过 FD
}

// 2. 辅助函数：创建一个新节点
static fd_node_t* create_fd_node() {
    fd_node_t* node = (fd_node_t*)kmalloc(sizeof(fd_node_t)); 
    if (!node) return nullptr;
    
    _memset(node->bitmap, 0, sizeof(node->bitmap));
    _memset(node->fds, 0, sizeof(node->fds));
    node->alloc_count = 0;
    node->next = nullptr;
    
    return node;
}

// 3. 分配一个空闲的 FD
int32_t fd_alloc(fd_manager_t* manager, fd_t** out_fd_ptr) {
    fd_node_t* curr = manager->head;
    fd_node_t* prev = nullptr;
    uint32_t node_index = 0; // 记录当前是第几个节点

    // 如果链表为空，创建第一个节点
    if (!curr) {
        manager->head = create_fd_node();
        curr = manager->head;
        if (!curr) return -1;
    }

    // 遍历链表寻找空闲位
    while (curr) {
        if (curr->alloc_count < FDS_PER_NODE) {
            for (int i = 0; i < 4; i++) {
                if (curr->bitmap[i] != ~(0ULL)) {
                    uint64_t inverted = ~(curr->bitmap[i]);
                    int bit_idx = __builtin_ctzll(inverted);
                    
                    // 标记为已分配
                    curr->bitmap[i] |= (1ULL << bit_idx);
                    curr->alloc_count++;
                    
                    // 隐式计算全局 FD 编号：(节点索引 * 256) + 内部偏移
                    int32_t global_fd = (node_index * FDS_PER_NODE) + (i * 64) + bit_idx;
                    
                    // 更新管理器中的最大 FD
                    if (global_fd > manager->max_fd) {
                        manager->max_fd = global_fd;
                    }
                    
                    if (out_fd_ptr) {
                        *out_fd_ptr = &curr->fds[(i * 64) + bit_idx];
                        (*out_fd_ptr)->filedesc = nullptr;
                        (*out_fd_ptr)->FSOPS = nullptr;
                        (*out_fd_ptr)->MP = nullptr;
                    }
                    
                    return global_fd;
                }
            }
        }
        node_index++;
        prev = curr;
        curr = curr->next;
    }

    // 所有现有节点都满了，创建新节点
    fd_node_t* new_node = create_fd_node();
    if (!new_node) return -1;
    
    prev->next = new_node;
    
    // 直接分配新节点的第 0 个位置
    new_node->bitmap[0] |= 1ULL;
    new_node->alloc_count = 1;
    
    int32_t global_fd = node_index * FDS_PER_NODE; // 新节点的第 0 个
    
    if (global_fd > manager->max_fd) {
        manager->max_fd = global_fd;
    }
    
    if (out_fd_ptr) {
        *out_fd_ptr = &new_node->fds[0];
    }
    
    return global_fd;
}

// 4. 根据 fd 编号获取 FileDesc 指针
fd_t* fd_get(fd_manager_t* manager, int32_t fd) {
    // 防御性编程：超出 max_fd 的绝对是非法句柄
    if (fd < 0 || fd > manager->max_fd) return nullptr;

    uint32_t target_node_index = fd / FDS_PER_NODE;
    uint32_t local_idx = fd % FDS_PER_NODE;
    
    fd_node_t* curr = manager->head;
    uint32_t curr_index = 0;
    
    // 快速前进到目标节点
    while (curr && curr_index < target_node_index) {
        curr = curr->next;
        curr_index++;
    }

    if (!curr) return nullptr; // 节点不存在

    uint32_t array_idx = local_idx / 64;
    uint32_t bit_idx   = local_idx % 64;

    // 安全检查：验证位图中是否真的标记为已分配
    if (!(curr->bitmap[array_idx] & (1ULL << bit_idx))) {
        return nullptr;
    }

    return &curr->fds[local_idx];
}

// 5. 释放 FD（关闭文件），并动态收缩 max_fd
void fd_free(fd_manager_t* manager, int32_t fd) {
    if (fd < 0 || fd > manager->max_fd) return;

    uint32_t target_node_index = fd / FDS_PER_NODE;
    uint32_t local_idx = fd % FDS_PER_NODE;
    
    fd_node_t* curr = manager->head;
    uint32_t curr_index = 0;
    
    while (curr && curr_index < target_node_index) {
        curr = curr->next;
        curr_index++;
    }

    if (!curr) return;

    uint32_t array_idx = local_idx / 64;
    uint32_t bit_idx   = local_idx % 64;

    // 如果确实是已分配状态，执行释放
    if (curr->bitmap[array_idx] & (1ULL << bit_idx)) {
        curr->bitmap[array_idx] &= ~(1ULL << bit_idx);
        curr->alloc_count--;
        
        curr->fds[local_idx].filedesc = nullptr;
        curr->fds[local_idx].FSOPS = nullptr;
        curr->fds[local_idx].MP = nullptr;
    }
    
    // ==========================================
    // 核心优化：如果释放的恰好是 max_fd，尝试收缩
    // ==========================================
    if (fd == manager->max_fd) {
        // 我们需要向回寻找新的 max_fd
        // 在实际的高性能内核中，如果不严格要求 max_fd 时刻精准，
        // 这一步可以省略（让 max_fd 只增不减，直到下一次从头分配）。
        // 但如果为了保持 max_fd 严格准确，我们需要倒序扫描位图。
        
        int32_t new_max = -1;
        fd_node_t* scan_node = manager->head;
        uint32_t scan_idx = 0;
        
        while (scan_node) {
            if (scan_node->alloc_count > 0) {
                // 找到当前节点中最高的为 1 的位
                // 这里为了简单直观采用遍历，其实也可以用 __builtin_clzll (Count Leading Zeros) 优化
                for (int i = 3; i >= 0; i--) {
                    if (scan_node->bitmap[i] != 0) {
                        // __builtin_clzll 找最高位的 1
                        int highest_bit = 63 - __builtin_clzll(scan_node->bitmap[i]);
                        int32_t node_max = (scan_idx * FDS_PER_NODE) + (i * 64) + highest_bit;
                        if (node_max > new_max) {
                            new_max = node_max;
                        }
                        break; // 这个 uint64 找到了，就不用看更低的了
                    }
                }
            }
            scan_node = scan_node->next;
            scan_idx++;
        }
        manager->max_fd = new_max;
    }
}

void fd_manager_destroy(fd_manager_t* manager) {
    if (!manager || !manager->head) return;

    fd_node_t* curr = manager->head;
    while (curr) {
        fd_node_t* next = curr->next;
        
        // ==========================================
        // 核心补全：遍历当前节点中所有已分配的 fd，执行 close 操作
        // ==========================================
        if (curr->alloc_count > 0) {
            for (int i = 0; i < 4; i++) {
                // 如果当前 64 位的位图不为 0，说明其中有被分配的 FD
                if (curr->bitmap[i] != 0) {
                    for (int bit_idx = 0; bit_idx < 64; bit_idx++) {
                        // 检查第 bit_idx 位是否被占用 (已分配)
                        if (curr->bitmap[i] & (1ULL << bit_idx)) {
                            uint32_t local_idx = (i * 64) + bit_idx;
                            fd_t* fd_ptr = &curr->fds[local_idx];
                            
                            // 防御性检查：确保底层操作接口及 close 指针合法
                            if (fd_ptr->FSOPS && fd_ptr->FSOPS->close) {
                                // 调用底层文件系统的 close 接口，传入对应的驱动私有描述符
                                fd_ptr->FSOPS->close(fd_ptr->filedesc);
                            }
                            
                            // 清空状态，防止悬垂指针引发 Use-After-Free
                            fd_ptr->filedesc = nullptr;
                            fd_ptr->FSOPS = nullptr;
                            fd_ptr->MP = nullptr;
                        }
                    }
                }
            }
        }
        
        // ==========================================
        
        kfree(curr); // 彻底释放当前节点的物理内存
        curr = next;
    }

    // 重置管理器状态
    manager->head = nullptr;
    manager->max_fd = -1;
}

} // extern "C"