//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <fs/fd.h>
#include <fs/lwext4/ext4.h>
#include <klib/algorithm/hmap.h>

#include <klib/klib.h>
#include <mem/heap.h>
#include <arch/x86_64/smp/smp.h>   /* round 94: this_cpu + cpu_t (fsync 激活) */

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

    // 寻找最小的可用 FD (P0-16: 分片写锁跨越 search+insert, 消除 TOCTOU 双插入)
    // 审计 #1 (round 1 修复): 原实现只对初始 hint 计算一次分片, new_fd 递增后
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

// 红黑树节点释放回调，用于 fd_manager_destroy
static void fd_destroy_cb(rb_node_t* node, void* arg) {
    fd_t* entry = container_of(node, fd_t, node);
    /* round 94: 进程退出 = 隐式关闭全部 FD —— 同样先冲刷脏缓存页
       (与 sys_fclose 对称; file_id = filedesc 指针, 键一致) */
    cpu_t *cpu = this_cpu();
    if (cpu->file_cache && entry->filedesc)
        file_cache_fsync(cpu->file_cache, (uint64_t)entry->filedesc);
    // 如果有底层文件描述符，调用其 close 方法
    if (entry->FSOPS && entry->FSOPS->close) {
        entry->FSOPS->close(entry->filedesc);
    }
    kfree(entry);
}

void fd_manager_destroy(fd_manager_t* manager) {
    if (!manager) return;
    
    /* round 90: 原 rb_sharded_clear 走 rb_postorder_iter —— 回调后读
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

/* P0-6 路线图 #7: fork 时 FD 继承 —— 快照式深拷贝。
   filedesc 按 FSOPS->SIZEOF_FILE_DESC 逐字节复制 (ext4_file 为纯句柄,
   复制后父子各持独立快照, 关闭互不干扰); 文件偏移 = fork 时刻快照,
   后续父子各自独立推进 (与 POSIX 共享偏移的偏差已文档化)。
   path 深拷贝; FSOPS/MP 为全局表指针, 直接共享。
   审计 #16 (round 7 修复): 同进程的其他线程可并发 fopen/fclose ——
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


int32_t file_cache_writeback_callback(
    uint64_t file_id,
    const uint8_t *key, 
    uint32_t key_len, void *data, size_t data_len
) {
    if (!key || key_len == 0 || !data || data_len == 0) return -1; /* 修复: 原为无值 return(UB) */

    /* 审计 #5 (round 5 修复): 原实现按 key(路径) 重新 open + O_TRUNC ——
       文件被删除后同名新文件会被截断 (数据写到错误文件)。现经
       file_id (= (uint64_t)filedesc, promote 时的原始句柄) 直写:
       同一 inode, 不重开路径 */
    if (file_id == 0) return -1;
    fd_t *orig = (fd_t *)file_id;
    if (!orig->FSOPS || !orig->filedesc) return -1;

    // 定位到开头后直写 (与路径重开版的 SEEK_SET 语义一致)
    orig->FSOPS->lseek(orig->filedesc, 0, 0);

    size_t wcnt = 0;
    int32_t err = orig->FSOPS->write(orig->filedesc, data, data_len, &wcnt);

    if (err != 0 || wcnt != data_len) {
        kinfo("[FC_WRITEBACK] Incomplete writeback (wrote %zu/%zu)\n", wcnt, data_len);
        /* 修复: 写入不完整必须报失败, 否则脏页被误判为已落盘 */
        return -1;
    }
    return 0;
}

} // extern "C"