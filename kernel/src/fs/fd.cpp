//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <fs/fd.h>
#include <fs/lwext4/ext4.h>
#include <klib/algorithm/hmap.h>



#include <klib/klib.h>
#include <mem/heap.h>

extern "C" char* GetMountPointName(const char* path) {
    if (!path || path[0] != '/' || path[1] == '/') 
        return nullptr;

    // 找到第一个有效字符后的第一个 '/'
    // 比如 "/mp/test" -> 找到 'p' 后面的那个 '/'
    const char* p = path + 1;
    //while (*p && *p == '/') p++; // 跳过开头重复的斜杠，如 "//mp" -> 指向 "mp"
    
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
static fd_node_t* create_fd_node() {
    fd_node_t* node = (fd_node_t*)kmalloc(sizeof(fd_node_t)); 
    if (!node) return nullptr;
    
    // 位图清0表示全部空闲
    _memset(node->bitmap, 0, sizeof(node->bitmap));
    _memset(node->fds, 0, sizeof(node->fds));
    node->alloc_count = 0;
    node->next = nullptr;
    
    return node;
}

// ===================== 外部接口实现 =====================

void fd_manager_init(fd_manager_t* manager) {
    manager->head = nullptr;
    manager->max_fd = -1;
    manager->lock = 0; // 初始化自旋锁
}

// 分配一个空闲的 FD
int32_t fd_alloc(fd_manager_t* manager, fd_t** out_fd_ptr) {
    //kinfoln("ALLOCATING FD");
    spinlock_lock(&manager->lock); // 加锁
    //kinfoln("ALLOCATING FD...");
    fd_node_t* curr = manager->head;
    fd_node_t* prev = nullptr;
    uint32_t node_index = 0; 
    
    // 如果链表为空，创建第一个节点
    if (!curr) {
        manager->head = create_fd_node();
        curr = manager->head;
        if (!curr) {
            spinlock_unlock(&manager->lock);
            return -1; // ENOMEM
        }
    }
    
    // 遍历链表寻找空闲位
    while (curr) {
        // alloc_count 提供快速判断，避免扫描满载节点的位图
        if (curr->alloc_count < FDS_PER_NODE) {
            for (int i = 0; i < 4; i++) {
                // 如果当前 64 位不全为 1，说明有空闲位
                if (curr->bitmap[i] != ~(0ULL)) {
                    // __builtin_ctzll 找到最低位的 0 变 1 的位置，极速分配
                    uint64_t inverted = ~(curr->bitmap[i]);
                    int bit_idx = __builtin_ctzll(inverted);
                    
                    // 标记为已分配
                    curr->bitmap[i] |= (1ULL << bit_idx);
                    curr->alloc_count++;
                    
                    // 计算全局 FD 编号
                    int32_t global_fd = (node_index * FDS_PER_NODE) + (i * 64) + bit_idx;
                    
                    // 更新最大 FD
                    if (global_fd > manager->max_fd) {
                        manager->max_fd = global_fd;
                    }
                    
                    // 返回 fd_t 指针给调用者填充
                    if (out_fd_ptr) {
                        *out_fd_ptr = &curr->fds[(i * 64) + bit_idx];
                        // 安全初始化，防止野指针
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

    // 所有现有节点都满了，创建新节点追加到链表尾部
    fd_node_t* new_node = create_fd_node();
    if (!new_node) {
        spinlock_unlock(&manager->lock);
        return -1;
    }
    
    prev->next = new_node;
    
    // 直接分配新节点的第 0 个位置
    new_node->bitmap[0] |= 1ULL;
    new_node->alloc_count = 1;
    
    int32_t global_fd = node_index * FDS_PER_NODE; 
    
    if (global_fd > manager->max_fd) {
        manager->max_fd = global_fd;
    }
    
    if (out_fd_ptr) {
        *out_fd_ptr = &new_node->fds[0];
        // 安全初始化
        (*out_fd_ptr)->filedesc = nullptr;
        (*out_fd_ptr)->FSOPS = nullptr;
        (*out_fd_ptr)->MP = nullptr;
    }
    
    spinlock_unlock(&manager->lock); // 解锁
    return global_fd;
}

// 根据 fd 编号获取 FileDesc 指针
fd_t* fd_get(fd_manager_t* manager, int32_t fd) {
    // 无需加锁，或者加读锁即可，因为 fd_get 只读不写
    if (fd < 0 || fd > manager->max_fd) return nullptr;

    uint32_t target_node_index = fd / FDS_PER_NODE;
    uint32_t local_idx = fd % FDS_PER_NODE;
    
    fd_node_t* curr = manager->head;
    
    // 快速前进到目标节点
    for (uint32_t i = 0; i < target_node_index && curr; ++i) {
        curr = curr->next;
    }

    if (!curr) return nullptr; // 节点不存在

    uint32_t array_idx = local_idx / 64;
    uint32_t bit_idx   = local_idx % 64;

    // 位图安全校验：该 FD 确实已被分配
    if (!(curr->bitmap[array_idx] & (1ULL << bit_idx))) {
        return nullptr;
    }

    return &curr->fds[local_idx];
}

// 释放 FD
void fd_free(fd_manager_t* manager, int32_t fd) {
    if (fd < 0) return;

    spinlock_lock(&manager->lock); // 加锁

    // 快速拦截非法 FD
    if (fd > manager->max_fd) {
        spinlock_unlock(&manager->lock);
        return;
    }

    uint32_t target_node_index = fd / FDS_PER_NODE;
    uint32_t local_idx = fd % FDS_PER_NODE;
    
    fd_node_t* curr = manager->head;
    
    for (uint32_t i = 0; i < target_node_index && curr; ++i) {
        curr = curr->next;
    }

    if (!curr) {
        spinlock_unlock(&manager->lock);
        return;
    }

    uint32_t array_idx = local_idx / 64;
    uint32_t bit_idx   = local_idx % 64;

    // 确认是已分配状态才执行清理
    if (curr->bitmap[array_idx] & (1ULL << bit_idx)) {
        curr->bitmap[array_idx] &= ~(1ULL << bit_idx);
        curr->alloc_count--;
        
        // 清理 fd_t 内部指针，防止悬垂指针
        curr->fds[local_idx].filedesc = nullptr;
        curr->fds[local_idx].FSOPS = nullptr;
        curr->fds[local_idx].MP = nullptr;
    }
    
    // 核心优化：O(1) 级别的 max_fd 收缩
    // 如果释放的是最高水位线，直接减1。
    // 即使减1后对应的FD并未分配，也毫无影响，fd_get 会通过位图安全拦截。
    if (fd == manager->max_fd) {
        manager->max_fd--;
    }

    spinlock_unlock(&manager->lock); // 解锁
}

// 销毁管理器，回收所有资源
void fd_manager_destroy(fd_manager_t* manager) {
    if (!manager || !manager->head) return;

    spinlock_lock(&manager->lock); // 加锁

    fd_node_t* curr = manager->head;
    while (curr) {
        fd_node_t* next = curr->next;
        
        // 遍历当前节点中所有已分配的 fd，执行 close 操作
        if (curr->alloc_count > 0) {
            for (int i = 0; i < 4; i++) {
                uint64_t bm = curr->bitmap[i];
                // 极速扫描：利用 __builtin_ctzll 提取已分配位，跳过连续的 0
                while (bm) {
                    int bit_idx = __builtin_ctzll(bm);
                    uint32_t local_idx = (i * 64) + bit_idx;
                    fd_t* fd_ptr = &curr->fds[local_idx];
                    
                    // 调用底层文件系统的 close 接口
                    if (fd_ptr->FSOPS && fd_ptr->FSOPS->close) {
                        fd_ptr->FSOPS->close(fd_ptr->filedesc);
                    }
                    
                    // 清除最低位的 1，进入下一个已分配的 FD
                    bm &= bm - 1; 
                }
            }
        }
        
        kfree(curr); // 释放节点内存
        curr = next;
    }

    // 重置管理器状态
    manager->head = nullptr;
    manager->max_fd = -1;

    spinlock_unlock(&manager->lock); // 解锁
}

} // extern "C"