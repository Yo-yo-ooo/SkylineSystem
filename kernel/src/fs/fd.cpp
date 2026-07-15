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

    const char* p = path + 1;
    const char* second_slash = strchr(p, '/');
    if (!second_slash) return nullptr;

    size_t len = (size_t)(second_slash - path) + 1;
    char* mp_name = (char*)kmalloc(len + 1);
    if (!mp_name) return nullptr; 

    __memcpy(mp_name, path, len);
    mp_name[len] = '\0';
    return mp_name;
}

extern "C" int32_t __hmap_s_mp_compare(const void* a, const void* b, void *udata) {
    return strcmp(((struct __hmap_s_mp*)a)->MPName, ((struct __hmap_s_mp*)b)->MPName);
}

extern "C" uint64_t __hmap_s_mp_hash(const void* item, uint64_t seed0, uint64_t seed1) {
    const struct __hmap_s_mp* entry = (const struct __hmap_s_mp*)item;
    return hashmap_sip(entry->MPName, strlen(entry->MPName), seed0, seed1);
}

extern "C" struct hashmap* HMapS_MP = nullptr;

void InitFFMAN() {
    HMapS_MP = hashmap_new(sizeof(struct __hmap_s_mp), 16, 0, 0,
        __hmap_s_mp_hash, __hmap_s_mp_compare, nullptr, nullptr);
}

spinlock_t hmap_mp_lock;

static __hmap_s_mp *GetMount_NoLock(const char *path) {
    if (!HMapS_MP || !path) return nullptr;
    char* mp_name = GetMountPointName(path);
    if (!mp_name) return nullptr;

    struct __hmap_s_mp temp_key = { .MPName = mp_name };
    __hmap_s_mp *hsmp = (__hmap_s_mp*)hashmap_get(HMapS_MP, &temp_key);

    kfree(mp_name);
    return hsmp;
}

__hmap_s_mp *GetMount(const char *path) {
    spinlock_lock(&hmap_mp_lock);
    __hmap_s_mp *ret = GetMount_NoLock(path);
    spinlock_unlock(&hmap_mp_lock);
    return ret;
}

// ===================== 红黑树管理 FD 的回调实现 =====================
extern "C" {

// 红黑树锁操作回调
static void fd_rb_lock(void* ctx) { spinlock_lock((spinlock_t*)ctx); }
static void fd_rb_unlock(void* ctx) { spinlock_unlock((spinlock_t*)ctx); }
static void fd_rb_rdlock(void* ctx) { spinlock_lock((spinlock_t*)ctx); }
static void fd_rb_rdunlock(void* ctx) { spinlock_unlock((spinlock_t*)ctx); }

static void* fd_rb_alloc_lock(uint32_t shard_idx) {
    spinlock_t* lock = (spinlock_t*)kmalloc(sizeof(spinlock_t));
    if (lock) *lock = 0;
    return lock;
}
static void fd_rb_free_lock(void* ctx) { kfree(ctx); }

// 红黑树内存分配回调
static void* fd_rb_alloc_mem(size_t size) { return kmalloc(size); }
static void fd_rb_free_mem(void* ptr) { kfree(ptr); }

// 红黑树 Key 提取与比较
static uint32_t fd_rb_hash(const void* key) {
    int32_t fd = *(const int32_t*)key;
    return rb_hash_u64((const void*)(uintptr_t)fd);
}

static const void* fd_rb_key_of(const rb_node_t* node) {
    fd_t* entry = container_of(node, fd_t, node);
    return &entry->fd;
}

static int fd_rb_cmp(const rb_node_t* a, const rb_node_t* b) {
    fd_t* fa = container_of(a, fd_t, node);
    fd_t* fb = container_of(b, fd_t, node);
    if (fa->fd < fb->fd) return -1;
    if (fa->fd > fb->fd) return 1;
    return 0;
}


void fd_manager_init(fd_manager_t* manager) {
    if (!manager) return;
    
    rb_shard_ops_t ops = {
        .hash_fn = fd_rb_hash,
        .key_of = fd_rb_key_of,
        .cmp = fd_rb_cmp
    };
    
    if (!rb_sharded_init(&manager->fd_tree, 4, &ops,
                         fd_rb_lock, fd_rb_unlock,
                         fd_rb_rdlock, fd_rb_rdunlock,
                         fd_rb_alloc_lock, fd_rb_free_lock,
                         fd_rb_alloc_mem, fd_rb_free_mem)) {
        Panic("FD Manager RBTree init failed!");
    }
    manager->next_fd_hint = 0;
}

int32_t fd_alloc(fd_manager_t* manager, fd_t** out_fd_ptr) {
    if (!manager) return -1;

    // 寻找最小的可用 FD
    int32_t new_fd = manager->next_fd_hint;
    fd_t search_key;
    search_key.fd = new_fd;
    
    // 如果当前 hint 被占用，则递增查找
    while (rb_sharded_search(&manager->fd_tree, &search_key.node)) {
        new_fd++;
        search_key.fd = new_fd;
    }

    // 分配新的 fd_t 结构体
    fd_t* new_entry = (fd_t*)kmalloc(sizeof(fd_t));
    if (!new_entry) return -1;
    
    rb_init_node(&new_entry->node);
    new_entry->fd = new_fd;
    new_entry->filedesc = nullptr;
    new_entry->FSOPS = nullptr;
    new_entry->MP = nullptr;

    // 插入红黑树
    rb_sharded_insert(&manager->fd_tree, &new_entry->node);

    // 更新下一次的探测起点
    manager->next_fd_hint = new_fd + 1;

    if (out_fd_ptr) {
        *out_fd_ptr = new_entry;
    }
    
    return new_fd;
}

fd_t* fd_get(fd_manager_t* manager, int32_t fd) {
    if (!manager || fd < 0) return nullptr;

    fd_t search_key;
    search_key.fd = fd;
    
    rb_node_t* node = rb_sharded_search(&manager->fd_tree, &search_key.node);
    if (!node) return nullptr;

    return container_of(node, fd_t, node);
}

void fd_free(fd_manager_t* manager, int32_t fd) {
    if (!manager || fd < 0) return;

    fd_t search_key;
    search_key.fd = fd;
    
    rb_node_t* node = rb_sharded_search(&manager->fd_tree, &search_key.node);
    if (node) {
        fd_t* entry = container_of(node, fd_t, node);
        
        // 从树中擦除
        rb_sharded_erase(&manager->fd_tree, node);
        
        // 释放 fd_t 内存
        kfree(entry);

        // 回收 FD 编号：允许下一次分配复用这个较小的 fd
        if (fd < manager->next_fd_hint) {
            manager->next_fd_hint = fd;
        }
    }
}

// 红黑树节点释放回调，用于 fd_manager_destroy
static void fd_destroy_cb(rb_node_t* node, void* arg) {
    fd_t* entry = container_of(node, fd_t, node);
    // 如果有底层文件描述符，调用其 close 方法
    if (entry->FSOPS && entry->FSOPS->close) {
        entry->FSOPS->close(entry->filedesc);
    }
    kfree(entry);
}

void fd_manager_destroy(fd_manager_t* manager) {
    if (!manager) return;
    
    // 遍历所有分片树，释放节点内存并关闭文件
    rb_sharded_clear(&manager->fd_tree, fd_destroy_cb, nullptr, fd_rb_free_lock);
    
    manager->next_fd_hint = 0;
}

} // extern "C"