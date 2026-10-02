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

    // 寻找最小的可用 FD (P0-16: 分片写锁跨越 search+insert, 消除 TOCTOU 双插入)
    int32_t new_fd = manager->next_fd_hint;
    fd_t search_key;
    search_key.fd = new_fd;
    rb_root_t *shard = rb_get_shard(&manager->fd_tree, &search_key.node);
    if (!shard) return -1;
    RB_WLOCK(shard);

    // 如果当前 hint 被占用，则递增查找 (锁内版本)
    while (rb_search_locked_only(shard, &search_key.node, manager->fd_tree.ops.cmp)) {
        new_fd++;
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

/* P0-6 路线图 #7: fork 时 FD 继承 —— 快照式深拷贝。
   filedesc 按 FSOPS->SIZEOF_FILE_DESC 逐字节复制 (ext4_file 为纯句柄,
   复制后父子各持独立快照, 关闭互不干扰); 文件偏移 = fork 时刻快照,
   后续父子各自独立推进 (与 POSIX 共享偏移的偏差已文档化)。
   path 深拷贝; FSOPS/MP 为全局表指针, 直接共享。
   调用前提: 源进程 = fork 的调用者自身, 单线程上下文, 无需加锁。 */
int32_t fd_manager_dup(fd_manager_t* dst, fd_manager_t* src) {
    if (!dst || !src) return -1;
    int32_t count = 0;
    for (uint32_t i = 0; i < src->fd_tree.shard_num; i++) {
        rb_root_t* shard = &src->fd_tree.shards[i];
        for (rb_node_t* n = rb_first(shard->node); n; n = rb_next(n)) {
            fd_t* se = container_of(n, fd_t, node);

            fd_t* ne = (fd_t*)kmalloc(sizeof(fd_t));
            if (!ne) continue;
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
                if (ne->path) {
                    __memcpy(ne->path, se->path, se->path_len);
                    ne->path[se->path_len] = '\0';
                }
            }
            if (se->filedesc && se->FSOPS && se->FSOPS->SIZEOF_FILE_DESC) {
                ne->filedesc = kmalloc(se->FSOPS->SIZEOF_FILE_DESC);
                if (ne->filedesc)
                    __memcpy(ne->filedesc, se->filedesc,
                             se->FSOPS->SIZEOF_FILE_DESC);
            }

            /* 目标树按同编号插入 (分片由 key 决定) */
            rb_root_t* dshard = rb_get_shard(&dst->fd_tree, &ne->node);
            if (!dshard) { kfree(ne); continue; }
            RB_WLOCK(dshard);
            rb_insert_raw(dshard, &ne->node, dst->fd_tree.ops.cmp);
            dshard->cnt++;
            RB_WUNLOCK(dshard);
            if (ne->fd >= dst->next_fd_hint) dst->next_fd_hint = ne->fd + 1;
            count++;
        }
    }
    return count;
}


int32_t file_cache_writeback_callback(
    const uint8_t *key, 
    uint32_t key_len, void *data, size_t data_len
) {
    if (!key || key_len == 0 || !data || data_len == 0) return -1; /* 修复: 原为无值 return(UB) */

    // 1. 分配内存并拷贝路径，确保以 '\0' 结尾
    char *kpath = (char *)kmalloc(key_len + 1);
    if (!kpath) return -1; /* 修复: 同上 */
    __memcpy(kpath, key, key_len);
    kpath[key_len] = '\0';

    // 2. 获取挂载点和文件系统操作表
    __hmap_s_mp *MP = GetMount(kpath);
    if (!MP) {
        kfree(kpath);
        return ENODATA; // 文件系统未挂载或路径无效，丢弃此脏页
    }

    // 3. 分配底层文件描述符结构
    void *filedesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
    if (!filedesc) {
        kfree(kpath);
        return ENOMEM;
    }
    _memset(filedesc, 0, MP->FSOPS->SIZEOF_FILE_DESC);

    // 4. 以只写 + 截断模式打开文件
    // P1-36a: 补 O_TRUNC —— 原实现只 O_WRONLY, 写回比旧文件短的数据
    // 时尾部残留旧内容 (脏页写回的语义 = 覆写整个缓存页)
    int32_t err = MP->FSOPS->open(filedesc, kpath, 0x1 | O_TRUNC); 
    if (err < 0) {
        kfree(filedesc);
        kfree(kpath);
        /* P1-36b: 删除文件的静默丢脏页 → 显式失败计入重试/FAILED
           (数据保留在缓存条目中, 由 P1-35 的冷却重试尝试恢复) */
        return EIO;
    }

    // 5. 将文件指针定位到开头 (SEEK_SET = 0)
    MP->FSOPS->lseek(filedesc, 0, 0);

    // 6. 将脏数据写回磁盘
    size_t wcnt = 0;
    err = MP->FSOPS->write(filedesc, data, data_len, &wcnt);
    
    if (err != 0 || wcnt != data_len) {
        kinfo("[FC_WRITEBACK] Incomplete writeback for %s (wrote %zu/%zu)\n", kpath, wcnt, data_len);
        /* 修复: 写入不完整必须报失败, 否则脏页被误判为已落盘 */
        MP->FSOPS->close(filedesc);
        kfree(filedesc);
        kfree(kpath);
        return -1;
    }

    // 7. 关闭文件并释放资源
    MP->FSOPS->close(filedesc);
    kfree(filedesc);
    kfree(kpath);
    return 0; /* 修复: 原函数无最终 return(UB, 调用方拿垃圾 eax 判成败) */
}

} // extern "C"