//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <fs/fd.h>
#include <klib/algorithm/rbtree.h>

extern volatile bool IsPM5LVL;
extern volatile pagemap_t* kernel_pagemap;

/* ============================================================
 *  缓存块与文件缓存结构定义
 * ============================================================ */
#define FC_BLOCK_DIRTY   0x01

typedef struct file_cache_block {
    rb_node_t   node;            // 红黑树节点
    uint64_t    offset;          // 文件偏移(块对齐)
    uint64_t    data_size;       // 块内有效数据字节数
    uint64_t    alloc_pages;     // 已分配物理页数
    void       *kaddr;           // 内核虚拟地址
    uint64_t    phys;            // 首页物理地址(用于 VMM::Map)
    uint8_t     flags;           // FC_BLOCK_DIRTY 等
} file_cache_block_t;

// 全局共享的文件缓存结构
typedef struct file_cache {
    rb_root_t   tree;            // 缓存块红黑树(按 offset 排序)
    spinlock_t  lock;            // 保护 tree 与元数据
    uint64_t    file_size;       // 当前文件总大小(可能动态增长)
    uint64_t    block_size;      // 缓存块最大值(由 file_size 决定)
    FS_PDESC    *FSOPS;           // 文件系统操作表
    int32_t     ref_count;       // 引用计数，记录有多少个 fd_t 在使用
    char        path[PATH_MAX];  // 文件绝对路径(作为全局哈希/树键)
} file_cache_t;

// 全局缓存索引树节点
typedef struct global_cache_entry {
    rb_node_t   node;
    char        path[PATH_MAX];
    file_cache_t *cache;
} global_cache_entry_t;

/* ============================================================
 *  全局缓存树管理 (实现多进程共享)
 * ============================================================ */
static rb_root_t global_cache_tree;
static spinlock_t global_cache_lock = 0;

static int global_cache_cmp(const rb_node_t *a, const rb_node_t *b) {
    const global_cache_entry_t *ea = RB_CONTAINER_OF(a, global_cache_entry_t, node);
    const global_cache_entry_t *eb = RB_CONTAINER_OF(b, global_cache_entry_t, node);
    return strcmp(ea->path, eb->path);
}

static int fc_block_cmp(const rb_node_t *a, const rb_node_t *b) {
    const file_cache_block_t *ca = RB_CONTAINER_OF(a, file_cache_block_t, node);
    const file_cache_block_t *cb = RB_CONTAINER_OF(b, file_cache_block_t, node);
    if (ca->offset < cb->offset) return -1;
    if (ca->offset > cb->offset) return  1;
    return 0;
}

// 缓存块最大值由文件大小决定
static uint64_t fc_determine_block_size(uint64_t file_size) {
    if (file_size <= (1ULL << 16)) return PAGE_SIZE;          /* 4KB */
    if (file_size <= (1ULL << 22)) return PAGE_SIZE * 16;     /* 64KB */
    if (file_size <= (1ULL << 28)) return PAGE_SIZE * 128;    /* 512KB */
    return PAGE_2MB;                                           /* 2MB */
}

// 分配单个缓存块 (物理页 + 内核映射)
static file_cache_block_t *fc_block_alloc(uint64_t offset, uint64_t alloc_size) {
    file_cache_block_t *blk = (file_cache_block_t *)kmalloc(sizeof(file_cache_block_t));
    if (!blk) return nullptr;

    rb_init_node(&blk->node);
    blk->offset = offset;
    blk->data_size = 0;
    blk->flags = 0;

    uint64_t pages = DIV_ROUND_UP(alloc_size, PAGE_SIZE);
    blk->alloc_pages = pages;

    void *kbuf = VMM::Alloc(kernel_pagemap, pages, false);
    if (!kbuf) {
        kfree(blk);
        return nullptr;
    }
    _memset(kbuf, 0, pages * PAGE_SIZE);

    blk->kaddr = kbuf;
    blk->phys = VMM::GetPhysics(kernel_pagemap, (uint64_t)kbuf);
    return blk;
}

static void fc_block_free(file_cache_block_t *blk) {
    if (!blk) return;
    VMM::Free(kernel_pagemap, blk->kaddr);
    kfree(blk);
}

static void fc_block_free_cb(rb_node_t *node, void *arg) {
    file_cache_block_t *blk = RB_CONTAINER_OF(node, file_cache_block_t, node);
    fc_block_free(blk);
}

// 回写脏块到磁盘
static void fc_block_flush(file_cache_t *cache, file_cache_block_t *blk, void *filedesc) {
    if (!(blk->flags & FC_BLOCK_DIRTY)) return;
    cache->FSOPS->lseek(filedesc, blk->offset, 0);
    size_t wcnt = 0;
    cache->FSOPS->write(filedesc, blk->kaddr, blk->data_size, &wcnt);
    blk->flags &= ~FC_BLOCK_DIRTY;
}

// 确保覆盖 offset 的块已缓存在全局树中并返回
static file_cache_block_t *fc_ensure_block(file_cache_t *cache, void *filedesc, uint64_t offset) {
    uint64_t block_offset = offset - (offset % cache->block_size);

    file_cache_block_t key;
    rb_init_node(&key.node);
    key.offset = block_offset;

    file_cache_block_t *blk = (file_cache_block_t *)rb_search(&cache->tree, &key.node, fc_block_cmp);
    if (blk) {
        rb_touch(&cache->tree, &blk->node);
        return blk;
    }

    // 未命中：分配新块并从磁盘读取
    blk = fc_block_alloc(block_offset, cache->block_size);
    if (!blk) return nullptr;

    if (block_offset < cache->file_size) {
        uint64_t to_read = cache->block_size;
        if (block_offset + to_read > cache->file_size)
            to_read = cache->file_size - block_offset;

        cache->FSOPS->lseek(filedesc, block_offset, 0);
        size_t rdcnt = 0;
        cache->FSOPS->read(filedesc, blk->kaddr, to_read, &rdcnt);
        blk->data_size = rdcnt;
    } else {
        blk->data_size = 0;
    }

    rb_insert(&cache->tree, &blk->node, fc_block_cmp);
    return blk;
}

// 从全局树获取或创建共享缓存
static file_cache_t *fc_get_or_create(const char *path, FS_PDESC *FSOPS, void *filedesc) {
    spinlock_lock(&global_cache_lock);

    global_cache_entry_t key_entry;
    rb_init_node(&key_entry.node);
    strcpy(key_entry.path, path);

    global_cache_entry_t *entry = (global_cache_entry_t *)rb_search(&global_cache_tree, &key_entry.node, global_cache_cmp);
    if (entry) {
        entry->cache->ref_count++;
        spinlock_unlock(&global_cache_lock);
        return entry->cache; // 找到共享缓存，直接返回
    }

    file_cache_t *cache = (file_cache_t *)kmalloc(sizeof(file_cache_t));
    if (!cache) {
        spinlock_unlock(&global_cache_lock);
        return nullptr;
    }

    rb_root_init(&cache->tree, nullptr, nullptr, nullptr, nullptr, nullptr);
    cache->lock = 0;
    cache->FSOPS = FSOPS;
    cache->ref_count = 1;
    strcpy(cache->path, path);

    cache->file_size = FSOPS->fsize(filedesc);
    cache->block_size = fc_determine_block_size(cache->file_size);

    entry = (global_cache_entry_t *)kmalloc(sizeof(global_cache_entry_t));
    if (!entry) {
        kfree(cache);
        spinlock_unlock(&global_cache_lock);
        return nullptr;
    }
    rb_init_node(&entry->node);
    strcpy(entry->path, path);
    entry->cache = cache;

    rb_insert(&global_cache_tree, &entry->node, global_cache_cmp);

    spinlock_unlock(&global_cache_lock);
    return cache;
}

// 释放全局缓存引用
static void fc_put(file_cache_t *cache) {
    spinlock_lock(&global_cache_lock);
    cache->ref_count--;
    if (cache->ref_count == 0) {
        global_cache_entry_t key_entry;
        rb_init_node(&key_entry.node);
        strcpy(key_entry.path, cache->path);

        global_cache_entry_t *entry = (global_cache_entry_t *)rb_search(&global_cache_tree, &key_entry.node, global_cache_cmp);
        if (entry) {
            rb_erase(&global_cache_tree, &entry->node);
        }
        spinlock_unlock(&global_cache_lock);

        // 没有进程再使用，彻底销毁缓存以释放物理内存
        spinlock_lock(&cache->lock);
        rb_clear(&cache->tree, fc_block_free_cb, nullptr);
        spinlock_unlock(&cache->lock);

        kfree(cache);
        if (entry) kfree(entry);
    } else {
        spinlock_unlock(&global_cache_lock);
    }
}

/* ============================================================
 *  用户窗口管理 (每个进程独立，映射到共享物理页)
 * ============================================================ */
static uint64_t fc_reserve_window(pagemap_t *pagemap, uint64_t pages) {
    spinlock_lock(&pagemap->vma_lock);
    uint64_t addr = VMM::VMA::InternalAlloc(pagemap, pages, MM_READ | MM_WRITE | MM_USER, 0);
    if (addr) {
        VMM::NewMapping(pagemap, addr, pages, MM_READ | MM_WRITE | MM_USER);
    }
    spinlock_unlock(&pagemap->vma_lock);
    return addr;
}

static void fc_release_window(pagemap_t *pagemap, uint64_t addr, uint64_t pages) {
    if (!addr) return;
    spinlock_lock(&pagemap->vma_lock);

    vma_region_t *region = VMM::VMA::FindRegion(pagemap, addr);
    if (region && region->start == addr) {
        pagemap->vma_cursor = region->prev;
        VMM::VMA::RemoveRegion(region);
    }

    vm_mapping_t *m = pagemap->vm_mappings;
    if (m) {
        vm_mapping_t *start_m = m;
        do {
            if (m->start == addr) {
                VMM::RemoveMapping(m);
                break;
            }
            m = m->next;
        } while (m != start_m);
    }
    spinlock_unlock(&pagemap->vma_lock);
}

/* ============================================================
 *  系统调用实现
 * ============================================================ */

uint64_t sys_fread(uint64_t fd_idx, uint64_t buf, uint64_t count, \
uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan, fd_idx);
    if (!FD) return -EBADF;
    
    if (count == 0) return 0;
    
    uint64_t user_space_end = IsPM5LVL ? USER_SPACE_END_5LVL : USER_SPACE_END_4LVL;
    if (buf >= user_space_end) return -EFAULT;
    if (count > user_space_end - buf) return -EFAULT;

    file_cache_t *cache = FD->cache;
    if (!cache) return -EIO;

    uint64_t pos = FD->FSOPS->lseek(FD->filedesc, 0, 1);

    if (pos >= cache->file_size) return 0;
    if (pos + count > cache->file_size) count = cache->file_size - pos;

    // 动态扩展当前进程的用户窗口
    if (cache->file_size > FD->window_pages * PAGE_SIZE) {
        uint64_t new_pages = ALIGN_UP(cache->file_size, PAGE_SIZE) / PAGE_SIZE;
        if (new_pages == 0) new_pages = 1;
        uint64_t new_base = fc_reserve_window(proc->pagemap, new_pages);
        if (!new_base) return -ENOMEM;
        if (FD->window_base) fc_release_window(proc->pagemap, FD->window_base, FD->window_pages);
        FD->window_base = new_base;
        FD->window_pages = new_pages;
    }

    spinlock_lock(&cache->lock);

    uint64_t remaining = count;
    uint64_t cur_pos = pos;
    uint64_t dst = buf;

    while (remaining > 0) {
        file_cache_block_t *blk = fc_ensure_block(cache, FD->filedesc, cur_pos);
        if (!blk) {
            spinlock_unlock(&cache->lock);
            return (count - remaining) > 0 ? (int64_t)(count - remaining) : -EIO;
        }

        // 将全局共享的物理页映射到当前进程的用户窗口
        uint64_t user_vaddr = FD->window_base + blk->offset;
        VMM::Useless::PageInfo info = VMM::Useless::GetPageInfo(proc->pagemap, user_vaddr);
        if (info.phys != blk->phys) {
            for (uint64_t i = 0; i < blk->alloc_pages; i++) {
                VMM::Map(proc->pagemap, user_vaddr + i * PAGE_SIZE, blk->phys + i * PAGE_SIZE, MM_READ | MM_WRITE | MM_USER);
            }
        }

        uint64_t in_block = cur_pos - blk->offset;
        uint64_t can_copy = blk->data_size - in_block;
        if (can_copy > remaining) can_copy = remaining;
        if (can_copy == 0) {
            spinlock_unlock(&cache->lock);
            return (count - remaining);
        }

        // 通过映射好的窗口地址拷贝数据给用户
        if (!VMM::UserAccess::CopyToUser(proc->pagemap, (void*)dst, (void*)(user_vaddr + in_block), can_copy)) {
            spinlock_unlock(&cache->lock);
            return -EFAULT;
        }

        cur_pos += can_copy;
        dst += can_copy;
        remaining -= can_copy;
    }

    spinlock_unlock(&cache->lock);
    FD->FSOPS->lseek(FD->filedesc, pos + count, 0);
    return count;
}

uint64_t sys_fwrite(uint64_t fd_idx, uint64_t buf, uint64_t count, \
uint64_t ign_0,uint64_t ign_1,uint64_t ign_2) {
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan, fd_idx);
    if (!FD) return -EBADF;
    
    if (count == 0) return 0;
    if (!is_user_buffer_valid(buf, count)) return -EFAULT;

    file_cache_t *cache = FD->cache;
    if (!cache) return -EIO;

    uint64_t pos = FD->FSOPS->lseek(FD->filedesc, 0, 1);

    if (pos + count > cache->file_size) {
        cache->file_size = pos + count;
    }

    // 动态扩展窗口
    if (cache->file_size > FD->window_pages * PAGE_SIZE) {
        uint64_t new_pages = ALIGN_UP(cache->file_size, PAGE_SIZE) / PAGE_SIZE;
        if (new_pages == 0) new_pages = 1;
        uint64_t new_base = fc_reserve_window(proc->pagemap, new_pages);
        if (!new_base) return -ENOMEM;
        if (FD->window_base) fc_release_window(proc->pagemap, FD->window_base, FD->window_pages);
        FD->window_base = new_base;
        FD->window_pages = new_pages;
    }

    spinlock_lock(&cache->lock);

    uint64_t remaining = count;
    uint64_t cur_pos = pos;
    uint64_t src = buf;

    while (remaining > 0) {
        file_cache_block_t *blk = fc_ensure_block(cache, FD->filedesc, cur_pos);
        if (!blk) {
            spinlock_unlock(&cache->lock);
            return (count - remaining) > 0 ? (int64_t)(count - remaining) : -EIO;
        }

        // 映射到当前进程窗口
        uint64_t user_vaddr = FD->window_base + blk->offset;
        VMM::Useless::PageInfo info = VMM::Useless::GetPageInfo(proc->pagemap, user_vaddr);
        if (info.phys != blk->phys) {
            for (uint64_t i = 0; i < blk->alloc_pages; i++) {
                VMM::Map(proc->pagemap, user_vaddr + i * PAGE_SIZE, blk->phys + i * PAGE_SIZE, MM_READ | MM_WRITE | MM_USER);
            }
        }

        uint64_t in_block = cur_pos - blk->offset;
        uint64_t can_copy = cache->block_size - in_block;
        if (can_copy > remaining) can_copy = remaining;

        if (!VMM::UserAccess::CopyFromUser(proc->pagemap, (void*)(user_vaddr + in_block), (void*)src, can_copy)) {
            spinlock_unlock(&cache->lock);
            return -EFAULT;
        }

        if (in_block + can_copy > blk->data_size) blk->data_size = in_block + can_copy;
        blk->flags |= FC_BLOCK_DIRTY;

        cur_pos += can_copy;
        src += can_copy;
        remaining -= can_copy;
    }

    spinlock_unlock(&cache->lock);
    FD->FSOPS->lseek(FD->filedesc, pos + count, 0);
    return count;
}

uint64_t sys_fsize(uint64_t fd_idx,GENERATE_IGN5()){
    IGNV_5();
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd_idx);
    if(!FD){return -EBADF;}
    if (FD->cache) return FD->cache->file_size;
    return FD->FSOPS->fsize(FD->filedesc);
}

uint64_t sys_flseek(uint64_t fd_idx, uint64_t offset, uint64_t whence, \
uint64_t ign_0,uint64_t ign_1,uint64_t ign_2){
    IGNORE_VALUE(ign_0);IGNORE_VALUE(ign_1);IGNORE_VALUE(ign_2);
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd_idx);
    if(!FD){return -EBADF;}
    
    if (whence > 2) {return -EINVAL; }
    if (whence == 2 && FD->cache) {
        uint64_t new_pos = FD->cache->file_size + offset;
        return FD->FSOPS->lseek(FD->filedesc, new_pos, 0);
    }
    return FD->FSOPS->lseek(FD->filedesc, offset, whence);
}

static int64_t strncpy_from_user(char* dst, const char* src, size_t max_len, pagemap_t* pagemap) {
    for (size_t i = 0; i < max_len; i++) {
        if (!VMM::UserAccess::CopyFromUser(pagemap, dst + i, (void*)(src + i), 1)) {
            return -EFAULT;
        }
        if (dst[i] == '\0') {
            return i;
        }
    }
    return -ENAMETOOLONG;
}

uint64_t sys_fopen(uint64_t path, uint64_t flags, GENERATE_IGN4()) {
    IGNV_4();
    proc_t *proc = Schedule::this_proc();

    if (!is_user_address(path)) { return -EFAULT; }

    char *kpath = (char *)kmalloc(PATH_MAX);
    if (!kpath) { return -ENOMEM; }

    int64_t path_len = strncpy_from_user(kpath, (const char*)path, PATH_MAX, proc->pagemap);
    if (path_len < 0) {
        kfree(kpath);
        return path_len;
    }
    
    __hmap_s_mp *MP = GetMount(kpath);
    if (!MP) {
        kfree(kpath);
        return -ENOENT; 
    }

    fd_t *fd_struct;
    int32_t fd_idx = fd_alloc(proc->FDMan, &fd_struct);
    if (fd_idx < 0) {
        kfree(kpath);
        return -EMFILE; 
    }

    fd_struct->FSOPS = MP->FSOPS;
    fd_struct->MP = MP;
    fd_struct->filedesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
    _memset(fd_struct->filedesc,0,MP->FSOPS->SIZEOF_FILE_DESC);
    fd_struct->cache = nullptr;
    fd_struct->window_base = 0;
    fd_struct->window_pages = 0;
    
    int32_t err = MP->FSOPS->open(fd_struct->filedesc, kpath, flags);

    if (err < 0) {
        kfree(kpath);
        kfree(fd_struct->filedesc);
        fd_free(proc->FDMan, fd_idx);
        return err; 
    }

    // 获取或创建全局共享缓存
    fd_struct->cache = fc_get_or_create(kpath, MP->FSOPS, fd_struct->filedesc);
    kfree(kpath);

    if (!fd_struct->cache) {
        MP->FSOPS->close(fd_struct->filedesc);
        kfree(fd_struct->filedesc);
        fd_free(proc->FDMan, fd_idx);
        return -ENOMEM;
    }

    // 为当前进程分配独立的用户窗口
    uint64_t win_bytes = ALIGN_UP(fd_struct->cache->file_size, PAGE_SIZE);
    if (win_bytes == 0) win_bytes = PAGE_SIZE;
    uint64_t win_pages = win_bytes / PAGE_SIZE;

    fd_struct->window_base = fc_reserve_window(proc->pagemap, win_pages);
    if (!fd_struct->window_base) {
        fc_put(fd_struct->cache);
        MP->FSOPS->close(fd_struct->filedesc);
        kfree(fd_struct->filedesc);
        fd_free(proc->FDMan, fd_idx);
        return -ENOMEM;
    }
    fd_struct->window_pages = win_pages;

    return (uint64_t)fd_idx;
}

uint64_t sys_fclose(uint64_t fd,GENERATE_IGN5()){
    IGNV_5();
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd);
    if(!FD){return -EBADF;}
    
    if (FD->cache) {
        // 进程关闭文件时，将自身的脏块回写到磁盘
        spinlock_lock(&FD->cache->lock);
        rb_node_t *cur = rb_first(FD->cache->tree.node);
        while (cur) {
            file_cache_block_t *blk = RB_CONTAINER_OF(cur, file_cache_block_t, node);
            fc_block_flush(FD->cache, blk, FD->filedesc);
            cur = rb_next(cur);
        }
        spinlock_unlock(&FD->cache->lock);

        // 释放本进程的用户窗口
        if (FD->window_base) {
            fc_release_window(proc->pagemap, FD->window_base, FD->window_pages);
        }

        // 释放全局缓存引用 (若降至0则释放物理内存)
        fc_put(FD->cache);
    }

    int32_t res = FD->FSOPS->close(FD->filedesc);
    kfree(FD->filedesc);
    fd_free(proc->FDMan,fd);
    return res;
}

uint64_t sys_mkdir(uint64_t path,uint64_t mode,GENERATE_IGN4()){
    IGNV_4();
    return -ENOSYS;
}