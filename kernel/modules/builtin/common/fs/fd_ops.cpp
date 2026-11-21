//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* fd_ops.cpp —— fd_alloc/fd_get/fd_free，迁入 kernel.fs.vfs.ko。
 * fd_manager_init/destroy/dup 仍留 Tier0（Schedule::Init/进程销毁直接调用）。 */
#include <fs/fd.h>
#include <mem/heap.h>
#include <klib/klib.h>

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

int32_t fd_alloc(fd_manager_t* manager, fd_t** out_fd_ptr) {
    if (!manager) return -1;

    // 寻找最小的可用 FD
    // 原实现只对初始 hint 计算一次分片, new_fd 递增后
    // 不重算 —— 锁/搜索/插入都在陈旧分片上, 条目插入与 fd_get 的分片计算
    // 错位 → FD 重复或不可见。修复: 每次递增后重取分片并重锁。
    int32_t new_fd = manager->next_fd_hint;
    fd_t search_key;
    search_key.fd = new_fd;
    rb_root_t *shard;
    for (;;) {
        /* Hash the KEY (&fd), not the node bytes. Passing &search_key.node
           hashed uninitialized stack memory, so the fd was inserted into a
           random shard that fd_get (which correctly hashes &fd) usually could
           not see -> intermittent -EBADF right after a successful open. */
        shard = rb_get_shard(&manager->fd_tree, &search_key.fd);
        if (!shard) return -1;
        RB_WLOCK(shard);
        if (!rb_search_locked_only(shard, &search_key.node, manager->fd_tree.ops.cmp))
            break;                       /* 该分片内此 fd 空闲 */
        RB_WUNLOCK(shard);
        new_fd++;                        /* 被占 → 递增并重算分片 */
        search_key.fd = new_fd;
    }

    // 分配新的 fd_t 结构体
    fd_t* new_entry = (fd_t*)kmalloc(sizeof(fd_t));
    if (!new_entry) { RB_WUNLOCK(shard); return -1; }
    
    rb_init_node(&new_entry->node);
    new_entry->fd = new_fd;
    new_entry->filedesc = nullptr;
    new_entry->FSOPS = nullptr;
    new_entry->MP = nullptr;

    // 插入红黑树 (锁内原始版本)
    rb_insert_raw(shard, &new_entry->node, manager->fd_tree.ops.cmp);
    shard->cnt++;
    RB_WUNLOCK(shard);

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

} // extern "C"
