//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <fs/fd.h>
#include <fs/fc.h>
#include <fs/lwext4/ext4.h>
#include <klib/algorithm/hmap.h>

#include <klib/klib.h>
#include <mem/heap.h>
#include <arch/x86_64/smp/smp.h>   /* this_cpu + cpu_t (fsync 激活) */


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


// 红黑树节点释放回调，用于 fd_manager_destroy
static void fd_destroy_cb(rb_node_t* node, void* arg) {
    fd_t* entry = container_of(node, fd_t, node);
    /* 进程退出 = 隐式关闭全部 FD —— 同样先冲刷脏缓存页
       (与 sys_fclose 对称; file_id = filedesc 指针, 键一致) */
    cpu_t *cpu = this_cpu();
    if (cpu->file_cache && entry->filedesc)
        file_cache_fsync_hook(cpu->file_cache, (uint64_t)entry->filedesc);
    // 如果有底层文件描述符，调用其 close 方法
    if (entry->FSOPS && entry->FSOPS->close) {
        entry->FSOPS->close(entry->filedesc);
    }
    kfree(entry);
}

void fd_manager_destroy(fd_manager_t* manager) {
    if (!manager) return;
    
    /* 原 rb_sharded_clear 走 rb_postorder_iter —— 回调后读
       tmp->right, fd_destroy_cb 若释放节点即 UAF (0xFFFFFFFF80024D18)。
       此前 FD 树恒空, 缺陷潜伏; fd_manager_dup 引入继承后引爆。
       改为 rb_erase_range 的"先取 next 再回调"模式 (全范围擦除) */
    for (uint32_t i = 0; i < manager->fd_tree.shard_num; i++) {
        rb_root_t* shard = &manager->fd_tree.shards[i];
        rb_erase_range(shard, nullptr, nullptr,
                       manager->fd_tree.ops.cmp, fd_destroy_cb, nullptr);
        fd_rb_free_lock(shard->lock_ctx);
        shard->lock_ctx = NULL;
    }
    manager->fd_tree.mem_free(manager->fd_tree.shards);
    manager->fd_tree.shards = NULL;
    manager->fd_tree.shard_num = 0;
    manager->fd_tree.shard_mask = 0;
    manager->fd_tree.ops.hash_fn = NULL;
    manager->fd_tree.ops.key_of  = NULL;
    manager->fd_tree.ops.cmp     = NULL;
    manager->fd_tree.mem_alloc   = NULL;
    manager->fd_tree.mem_free    = NULL;

    manager->next_fd_hint = 0;
}

/* 路线图 fork 时 FD 继承 —— 快照式深拷贝。
   filedesc 按 FSOPS->SIZEOF_FILE_DESC 逐字节复制 (ext4_file 为纯句柄,
   复制后父子各持独立快照, 关闭互不干扰); 文件偏移 = fork 时刻快照,
   后续父子各自独立推进 (与 POSIX 共享偏移的偏差已文档化)。
   path 深拷贝; FSOPS/MP 为全局表指针, 直接共享。
   同进程的其他线程可并发 fopen/fclose ——
   遍历持源分片读锁; OOM 不再静默跳过 (空 filedesc 会让子进程的
   close 崩溃), 中止并返回 -1, 调用方 (Fork) 整体回滚 */
int32_t fd_manager_dup(fd_manager_t* dst, fd_manager_t* src) {
    if (!dst || !src) return -1;
    int32_t count = 0;
    for (uint32_t i = 0; i < src->fd_tree.shard_num; i++) {
        rb_root_t* shard = &src->fd_tree.shards[i];
        RB_RDLOCK(shard);
        for (rb_node_t* n = rb_first(shard->node); n; n = rb_next(n)) {
            fd_t* se = container_of(n, fd_t, node);

            fd_t* ne = (fd_t*)kmalloc(sizeof(fd_t));
            if (!ne) { RB_RDUNLOCK(shard); return -1; }
            rb_init_node(&ne->node);
            ne->fd = se->fd;
            ne->path = nullptr;
            ne->path_len = se->path_len;
            ne->file_size = se->file_size;
            ne->FSOPS = se->FSOPS;
            ne->MP = se->MP;
            ne->filedesc = nullptr;
            /* 复制的是同一个打开实例: 沿用 file_uid, 块缓存可以共享
               (只读块)。若也复制 offset, 父子共享同一个读写位置。 */
            ne->offset   = se->offset;
            ne->file_uid = se->file_uid;

            if (se->path) {
                ne->path = (char*)kmalloc(se->path_len + 1);
                if (!ne->path) { kfree(ne); RB_RDUNLOCK(shard); return -1; }
                __memcpy(ne->path, se->path, se->path_len);
                ne->path[se->path_len] = '\0';
            }
            if (se->filedesc && se->FSOPS && se->FSOPS->SIZEOF_FILE_DESC) {
                ne->filedesc = kmalloc(se->FSOPS->SIZEOF_FILE_DESC);
                if (!ne->filedesc) { kfree(ne->path); kfree(ne); RB_RDUNLOCK(shard); return -1; }
                __memcpy(ne->filedesc, se->filedesc,
                         se->FSOPS->SIZEOF_FILE_DESC);
            }

            /* 目标树按同编号插入 (分片由 key 决定) */
            rb_root_t* dshard = rb_get_shard(&dst->fd_tree, &ne->node);
            if (!dshard) { kfree(ne->filedesc); kfree(ne->path); kfree(ne); RB_RDUNLOCK(shard); return -1; }
            RB_WLOCK(dshard);
            rb_insert_raw(dshard, &ne->node, dst->fd_tree.ops.cmp);
            dshard->cnt++;
            RB_WUNLOCK(dshard);
            if (ne->fd >= dst->next_fd_hint) dst->next_fd_hint = ne->fd + 1;
            count++;
        }
        RB_RDUNLOCK(shard);
    }
    return count;
}



} // extern "C"