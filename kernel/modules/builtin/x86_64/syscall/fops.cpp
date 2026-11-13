//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#include <arch/x86_64/schedule/sched.h>
#include <arch/x86_64/schedule/syscall.h>
#include <klib/errno.h>
#include <elf/elf.h>
#include <mem/pmm.h>
#include <fs/fd.h>
#include <fs/fc.h>

extern volatile bool IsPM5LVL;

uint64_t sys_fread(uint64_t fd_idx, uint64_t buf, uint64_t count, \
GENERATE_IGN3()) {
    IGNV_3();
    
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan, fd_idx);
    if(!FD) { return -EBADF; }
    
    if (count == 0) return 0;
    
    uint64_t user_space_end = IsPM5LVL ? USER_SPACE_END_5LVL : USER_SPACE_END_4LVL;
    if (buf >= user_space_end) return -EFAULT;
    if (count > user_space_end - buf) return -EFAULT;

    file_cache_entry_t *cache_entry = NULL;
    size_t out_len = 0;
    cpu_t *cpu = this_cpu();
    
    /* 由内核记账当前偏移 (lwext4 的 lseek 返回状态码, 不能用来查询位置) */
    uint64_t cur_offset = FD->offset;
    
    void *cached_data = file_cache_get(cpu->file_cache,(const uint8_t*)FD->path, FD->path_len, count, &out_len, &cache_entry);
    
    if (cached_data) {
        // 严格判断：读取的范围必须在缓存覆盖的数据范围之内
        if (cur_offset + count <= out_len) {
            // 缓存命中，且偏移量安全。直接将对应偏移的数据拷贝给用户态
            if (!VMM::UserAccess::CopyToUser(proc->pagemap, buf, (void*)((uint64_t)cached_data + cur_offset), count)) {
                file_cache_put(cpu->file_cache, cache_entry);
                return -EFAULT;
            }
            file_cache_put(cpu->file_cache, cache_entry);
            
            // 读取成功后，推进文件描述符的偏移量
            FD->FSOPS->lseek(FD->filedesc, cur_offset + count, SEEK_SET);
            FD->offset = cur_offset + count;
            file_cache_record_io(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len, count, NULL, FD->file_size, FD->file_uid);
            return count;
        }
        // 读取范围超出缓存范围，放弃缓存，回退到硬件读取
        file_cache_put(cpu->file_cache, cache_entry);
    } else {
        /* Step 2: path 键整文件未命中时, 尝试逐块命中 ——
           任意 offset 的读都能命中已 promote 的块 (非零 offset 的
           promote 见下方 miss 分支) */
        const uint64_t BLOCK_SZ = 4096;
        uint64_t blocks = (count + BLOCK_SZ - 1) / BLOCK_SZ;
        uint64_t first_block = cur_offset / BLOCK_SZ;
        bool all_hit = (count > 0);
        for (uint64_t b = 0; b < blocks; b++) {
            file_cache_entry_t *be = NULL;
            size_t blen = 0;
            void *bdata = file_cache_get_block(cpu->file_cache,
                FD->file_uid, first_block + b,
                count, &blen, &be);
            if (!bdata) { all_hit = false; break; }
            /* 命中语义: 缓存块 = 该文件第 (first_block+b) 块**自块首起**
               的 blen 个字节。所以 from 必须落在 [0, blen) 内且整段读都要
               在这段已知前缀之内, 否则放弃缓存走硬件。 */
            size_t from = (size_t)((cur_offset + b * BLOCK_SZ) % BLOCK_SZ);
            size_t chunk = count - b * BLOCK_SZ;
            if (from >= blen) { all_hit = false; file_cache_put(cpu->file_cache, be); break; }
            if (chunk > blen - from) { all_hit = false; file_cache_put(cpu->file_cache, be); break; }
            if (!VMM::UserAccess::CopyToUser(proc->pagemap,
                    buf + b * BLOCK_SZ,
                    (void*)((uint64_t)bdata + from), chunk)) {
                file_cache_put(cpu->file_cache, be);
                return -EFAULT;
            }
            file_cache_put(cpu->file_cache, be);
        }
        if (all_hit) {
            FD->FSOPS->lseek(FD->filedesc, cur_offset + count, SEEK_SET);
            FD->offset = cur_offset + count;
            file_cache_record_io(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len, count, NULL, FD->file_size, FD->file_uid);
            return count;
        }
    }

    // Cache Miss: Hardware
    /* 中转缓冲按 count 分配; 内存紧张时 kmalloc 可能失败。
       原实现直接返回 -ENOMEM, 而上层 (libc 的 fread 循环) 拿到 0 就
       **提前结束**, 表现为"只读了一半"的静默截断 —— 实测 64/72 MiB 下
       DOOM 报 `W_ReadLump: only read 8192 of 17544 on lump 1955`,
       整局游戏起不来。
       改为降级: 分配不到就把本次请求缩小到能分配的最大块, 返回**部分读**。
       调用方的循环会继续, 只是多几次系统调用。 */
    /* 中转缓冲优先用**每 CPU 静态 8 KiB** (不占堆、不会失败), 但同一 CPU 上
       可能被抢占重入 (FS 读会阻塞), 所以配一个占用标志: 缓冲已忙时退回
       内核堆分配 (分配不到再逐级缩小, 返回部分读而不是 -ENOMEM)。
       只用静态缓冲的版本实测会因重入踩坏数据: 72 MiB 下 DOOM 崩溃 +
       STALL WATCHDOG (等待 FFFFFFFF80025F81 处的锁)。 */
    static char s_read_bounce[MAX_CPU][8192];
    static volatile int s_bounce_busy[MAX_CPU];
    cpu_t *rcpu = this_cpu();
    uint32_t rcid = rcpu ? (uint32_t)rcpu->id : 0u;
    if (rcid >= MAX_CPU) rcid = 0;
    const size_t bounce_cap = sizeof(s_read_bounce[0]);

    void *kbuf = nullptr;
    bool use_static = false;
    if (count <= bounce_cap &&
        !__atomic_test_and_set(&s_bounce_busy[rcid], __ATOMIC_ACQUIRE)) {
        kbuf = s_read_bounce[rcid];
        use_static = true;
    } else {
        kbuf = kmalloc(count);
        if (!kbuf) {
            size_t shrink = (size_t)count;
            while (shrink > 512) {
                shrink >>= 1;
                kbuf = kmalloc(shrink);
                if (kbuf) break;
            }
            if (!kbuf) return -ENOMEM;
            count = shrink;
        }
    }
    bool big_read = (count > bounce_cap);

    if (big_read) {
        /* 大读: 逐 8 KiB 块读入并直接拷给用户, 不建立缓存前缀。 */
        size_t done = 0;
        while (done < (size_t)count) {
            size_t n = (size_t)count - done;
            if (n > bounce_cap) n = bounce_cap;
            FD->FSOPS->lseek(FD->filedesc, cur_offset + done, SEEK_SET);
            size_t got = 0;
            FD->FSOPS->read(FD->filedesc, kbuf, n, &got);
            if (got == 0) break;
            if (!VMM::UserAccess::CopyToUser(proc->pagemap, buf + done, kbuf, got)) {
                if (done == 0) return -EFAULT;
                break;
            }
            done += got;
            if (got < n) break;
        }
        if (use_static) __atomic_clear(&s_bounce_busy[rcid], __ATOMIC_RELEASE);
        if (done > 0) FD->offset = cur_offset + done;
        return done;                        /* 部分读也是合法的进展 */
    }
    
    // 确保文件偏移量正确（前面 lseek 可能未改变，但保险起见恢复状态）
    FD->FSOPS->lseek(FD->filedesc, cur_offset, SEEK_SET);

    size_t total_read = 0;
    FD->FSOPS->read(FD->filedesc, kbuf, count, &total_read);
    
    if (total_read > 0) {
        
        if (!VMM::UserAccess::CopyToUser(proc->pagemap, buf, kbuf, total_read)) {
            /* kbuf 是静态中转缓冲, 不能 kfree */
            return -EFAULT;
        }

        /* 大文件不做块级缓存（DOOM WAD 11MB、字体等）：每 4KiB 一个条目会让
         * 缓存/淘汰长期高 churn，并在 SMP 下暴露 file_cache 条目生命周期竞态
         * （实测 DOOM 启动阶段偶发 file_cache_promote invalid free）。小文件
         * （程序/配置/主题）仍然正常缓存；大文件交给块设备/页缓存前的读取。
         * 这是稳定性优先的有意取舍，见 docs/desktop-features.md §8。 */
        const bool cache_promote_ok =
            (FD->file_size == 0 || FD->file_size <= (1ull << 20));

        // 只有从 offset 0 开始读取的数据，才能安全作为整个文件的缓存前缀进行记录/提升
        if (cur_offset == 0) {
            if (cache_promote_ok) {
                void *cache_buf = kmalloc(total_read);
                if (cache_buf) {
                    __memcpy(cache_buf, kbuf, total_read);
                    file_cache_record_io(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len, total_read, cache_buf, FD->file_size, FD->file_uid);
                }
            } else {
                file_cache_record_io(cpu->file_cache, (const uint8_t*)FD->path,
                                     FD->path_len, total_read, NULL,
                                     FD->file_size, FD->file_uid);
            }
        } else {
            /* Step 2: 非零偏移读取改为逐块 promote ——
               原实现直接放弃缓存, 任意 offset 的读都无法建立缓存。
               按 4KB 块切分读到的数据, 每块以 (file_id, block#) 键
               promote (块级缓存, fc-block-cache-design.md) */
            const uint64_t BLOCK_SZ = 4096;
            uint64_t off = 0;
            while (off < total_read) {
                uint64_t foff     = cur_offset + off;
                uint64_t blk_from = foff % BLOCK_SZ;      /* 本次在块内的起点 */
                uint64_t block_no = foff / BLOCK_SZ;
                size_t in_block = (size_t)(BLOCK_SZ - blk_from);
                size_t chunk = (total_read - off < in_block) ?
                    (total_read - off) : in_block;

                /* 只缓存**从块首开始**的块。命中路径按 (file, block#) 取块
                   后用调用方的块内偏移直接索引, 隐含"数据是块前缀"; 若把
                   块内偏移 2644 处的 54 字节当块 promote, 同块内其它偏移
                   的读就会命中并拿到错位数据 —— 密集的小块读 (逐字形读
                   glyf 切片, 每 4KB 块里约 8 个字形) 必然连环踩中, 表现为
                   文件内容随机损坏。非块首的部分块跳过, 后续块仍然入缓存。 */
                if (blk_from == 0 && cache_promote_ok) {
                    void *block_buf = kmalloc(chunk);
                    if (!block_buf) break;   /* OOM: 放弃剩余块, 已 promote 的保留 */
                    __memcpy(block_buf, (uint8_t*)kbuf + off, chunk);
                    file_cache_promote_block(cpu->file_cache,
                        FD->file_uid, block_no,
                        block_buf, chunk, false, FD->file_size);
                }
                off += chunk;
            }
            // 仍记录整次 IO 统计以供启发式策略使用
            file_cache_record_io(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len, total_read, NULL, FD->file_size, FD->file_uid);
        }
    }

    if (use_static) __atomic_clear(&s_bounce_busy[rcid], __ATOMIC_RELEASE);
    else kfree(kbuf);
    if (total_read > 0) FD->offset = cur_offset + total_read;
    return (int64_t)total_read;
}

uint64_t sys_fsize(uint64_t fd_idx,GENERATE_IGN5()){
    IGNV_5();
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd_idx);
    if(!FD){return -EBADF;}
    return FD->FSOPS->fsize(FD->filedesc);
}

uint64_t sys_fwrite(uint64_t fd_idx, uint64_t buf, uint64_t count, \
GENERATE_IGN3()) {
    IGNV_3();
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd_idx);
    if(!FD){return -EBADF;}
    
    if (count == 0) return 0;
    
    if (!is_user_buffer_valid(buf, count)) {
        return -EFAULT;
    }

    void* kbuf = kmalloc(count);
    if (!kbuf) return -ENOMEM;

    if (!VMM::UserAccess::CopyFromUser(proc->pagemap, kbuf, (void*)buf, count)) {
        kfree(kbuf);
        return -EFAULT;
    }

    // 获取写入前的偏移量 (内核记账: lseek 返回的是状态码, 不能用来查询)
    uint64_t cur_offset = FD->offset;

    size_t wcnt = 0;
    int32_t status = FD->FSOPS->write(FD->filedesc, kbuf, count, &wcnt);
    
    if (status == 0 && wcnt > 0) {
        cpu_t *cpu = this_cpu();
        size_t out_len = 0;
        file_cache_entry_t *cache_entry = NULL;
        
        // 尝试获取缓存
        void *cached_data = file_cache_get(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len, wcnt, &out_len, &cache_entry);
        
        if (cached_data) {
            // 缓存命中修改逻辑：如果写入的范围完全在缓存覆盖范围内
            if (cur_offset + wcnt <= out_len) {
                // 直接在内存中修改缓存对应位置的数据 (Write-through 策略)
                __memcpy((void*)((uint64_t)cached_data + cur_offset), kbuf, wcnt);
                // 因为数据已经同步写入磁盘了，所以不需要标记脏页，缓存与磁盘保持一致
                /* 修复: 原地更新后必须跨 CPU 失效, 否则其他核的缓存条目
                   仍持有旧字节(陈旧读) */
                file_cache_invalidate(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len);
                /* Step 3: 同时失效写入范围覆盖的块条目 ——
                   块级条目与 path 条目并存, 只失效 path 会让块条目
                   保持陈旧字节 */
                const uint64_t BLOCK_SZ = 4096;
                uint64_t wb0 = cur_offset / BLOCK_SZ;
                uint64_t wb1 = (cur_offset + wcnt - 1) / BLOCK_SZ;
                for (uint64_t wb = wb0; wb <= wb1; wb++)
                    file_cache_invalidate_block(cpu->file_cache,
                        FD->file_uid, wb);
            } else {
                // 写入超出了缓存范围，现有缓存不再能代表文件前缀，使其失效
                file_cache_put(cpu->file_cache, cache_entry);
                file_cache_invalidate(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len);
                cache_entry = NULL;
            }
            if (cache_entry) {
                file_cache_put(cpu->file_cache, cache_entry);
            }
            file_cache_record_io(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len, wcnt, NULL, FD->file_size, FD->file_uid);
        } else {
            // 缓存未命中。如果是从 offset 0 开始写，则可以将这批数据作为新的缓存块 promote
            if (cur_offset == 0) {
                void *cache_buf = kmalloc(wcnt);
                if (cache_buf) {
                    __memcpy(cache_buf, kbuf, wcnt);
                    // is_dirty = false，因为 FSOPS->write 已经同步落盘
                    int32_t r = file_cache_promote(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len, cache_buf, wcnt, false, FD->file_size, FD->file_uid);
                    if (r != 0) kfree(cache_buf); 
                }
            } else {
                /* Step 3: 非零偏移写同样逐块 promote (与读
                   路径对称) —— 写已同步落盘, is_dirty=false */
                const uint64_t BLOCK_SZ = 4096;
                uint64_t off = 0;
                while (off < wcnt) {
                    uint64_t foff     = cur_offset + off;
                    uint64_t blk_from = foff % BLOCK_SZ;
                    uint64_t block_no = foff / BLOCK_SZ;
                    size_t in_block = (size_t)(BLOCK_SZ - blk_from);
                    size_t chunk = (wcnt - off < in_block) ?
                        (wcnt - off) : in_block;
                    /* 与读路径同一条不变量: 只缓存自块首起的块前缀 */
                    if (blk_from == 0) {
                        void *block_buf = kmalloc(chunk);
                        if (!block_buf) break;
                        __memcpy(block_buf, (uint8_t*)kbuf + off, chunk);
                        file_cache_promote_block(cpu->file_cache,
                            FD->file_uid, block_no,
                            block_buf, chunk, false, FD->file_size);
                    }
                    off += chunk;
                }
                file_cache_record_io(cpu->file_cache, (const uint8_t*)FD->path, FD->path_len, wcnt, NULL, FD->file_size, FD->file_uid);
            }
        }
        
        // 修复 Bug 5: 不能盲目 +=，必须从文件系统获取真实大小
        FD->file_size = FD->FSOPS->fsize(FD->filedesc);
        FD->offset = cur_offset + wcnt;
    }

    kfree(kbuf);

    if (status != 0) return (int64_t)status;
    return (int64_t)wcnt;
}

uint64_t sys_flseek(uint64_t fd_idx, uint64_t offset, uint64_t whence, \
GENERATE_IGN3()){
    IGNV_3();
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd_idx);
    if(!FD){return -EBADF;}
    
    if (whence > 2) {return -EINVAL; }
    int32_t lr = FD->FSOPS->lseek(FD->filedesc, offset, whence);
    if (lr != 0) return (uint64_t)(int64_t)(lr > 0 ? -lr : lr);

    /* 记账新的绝对偏移: SEEK_SET=0 / SEEK_CUR=1 / SEEK_END=2 */
    uint64_t base = (whence == SEEK_SET) ? 0u
                  : (whence == SEEK_CUR) ? FD->offset
                  : FD->file_size;
    uint64_t np = base + offset;
    if (np > FD->file_size) np = FD->file_size;
    FD->offset = np;
    return np;
}

static inline bool is_path_too_long(const char* kpath) {
    const uint64_t* p = (const uint64_t*)kpath;
    const uint64_t* end = (const uint64_t*)(kpath + PATH_MAX);

    while (p < end) {
        uint64_t v = *p;
        if (((v - 0x0101010101010101ULL) & ~v & 0x8080808080808080ULL)) {
            return false;
        }
        p++;
    }
    return true;
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

    if (!is_user_address(path)) { 
        return -EFAULT; 
    }

    char *kpath = (char *)kmalloc(PATH_MAX);
    if (!kpath) { 
        return -ENOMEM; 
    }

    int64_t path_len = strncpy_from_user(kpath, (const char*)path, PATH_MAX, proc->pagemap);
    if (path_len < 0) {
        kinfoln("sys_fopen free kpath(pathlen < 0)");
        kfree(kpath);
        return path_len;
    }
    
    __hmap_s_mp *MP = GetMount(kpath);
    if (!MP) {
        kinfoln("sys_fopen free kpath(!MP)");
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
    fd_struct->offset = 0;          /* 内核记账的文件偏移从 0 开始 */
    fd_struct->file_uid = fc_next_file_uid();
    fd_struct->filedesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
    _memset(fd_struct->filedesc,0,MP->FSOPS->SIZEOF_FILE_DESC);
    
    int32_t err = MP->FSOPS->open(fd_struct->filedesc, kpath, flags);

    /* 文件系统层的返回约定是 0(EOK) 成功、其余为**正** errno
       (lwext4: ENOENT=2, EIO=5), 而系统调用 ABI 是**负** errno。
       这里原来写成 `err < 0`, 于是"打开一个不存在的文件"会被当成成功
       并返回一个有效 fd; 该 fd 的 ext4_file 从未被真正打开 (mp==0),
       后续的 fsize/read/close 立刻踩中 ext4_assert(file && file->mp)
       导致内核 panic。任何"探测文件是否存在"的用户态程序都会触发,
       DOOM 的 WAD 探测 (依次尝试 6 个 wad 名) 是第一个踩到的。
       exec.cpp / task.cpp 用的都是 `!= 0`, 这里对齐它们。 */
    if (err != 0) {
        kfree(fd_struct->filedesc);
        fd_free(proc->FDMan, fd_idx);
        kfree(kpath);
        return (uint64_t)(int64_t)(err > 0 ? -err : err);
    }

    // 修复 Bug 6: 额外分配1字节用于 NUL 终止符，防止越界读
    fd_struct->path = (char*)kmalloc(path_len + 1);
    if (fd_struct->path) {
        __memcpy(fd_struct->path, kpath, path_len);
        fd_struct->path[path_len] = '\0'; // 显式添加 NUL
        fd_struct->path_len = path_len;
        fd_struct->file_size = MP->FSOPS->fsize(fd_struct->filedesc); 
    } else {
        
        MP->FSOPS->close(fd_struct->filedesc);
        kfree(fd_struct->filedesc);
        fd_free(proc->FDMan, fd_idx);
        kfree(kpath);
        return -ENOMEM;
    }

    kfree(kpath); 

    return (uint64_t)fd_idx;
}

uint64_t sys_fclose(uint64_t fd,GENERATE_IGN5()){
    IGNV_5();
    proc_t *proc = Schedule::this_proc();
    fd_t *FD = fd_get(proc->FDMan,fd);
    if(!FD){return -EBADF;}
    /* 关闭前冲刷该文件的脏缓存页 ——
       file_id = (uint64_t)filedesc 与 fops 的 promote/record_io 键一致;
       fsync 的写回回调按路径重开文件落盘, 失败不丢脏 (fc 的重试机制)。
       这才让 "fsync 机制存在但生产未激活" 变成已激活 */
    cpu_t *cpu = this_cpu();
    if (cpu->file_cache && FD->filedesc)
        file_cache_fsync(cpu->file_cache, FD->file_uid);
    int32_t res = FD->FSOPS->close(FD->filedesc);
    
    if (FD->path) {
        kfree(FD->path);
        FD->path = NULL;
    }

    fd_free(proc->FDMan,fd);
    return res;
}

/* ============================================================
 * sys_mkdir —— 固定 syscall 22（ABI: path, mode）
 * ------------------------------------------------------------
 * 目录创建（Setup 安装器用它建 /mp/system/ 下的目录树）。
 *   - 只支持 ext4 挂载：SAF/FAT 等实现返回 -ENOSYS（诚实语义，不假装成功）；
 *   - ext4_dir_mk() 对已存在的目录返回 EOK（幂等），所以调用方可以按路径
 *     逐段无脑 mkdir；
 *   - 返回 0 成功；负 errno（-ENOENT 无挂载点 / -ENOMEM / -EFAULT / -EIO…）。
 * 注意：lwext4 的目录创建走同一挂载点锁与事务，本函数不额外持锁。
 * ============================================================ */
uint64_t sys_mkdir(uint64_t path, uint64_t mode, GENERATE_IGN4()) {
    IGNV_4();
    (void)mode;   /* SkylineSystem 目前没有 per-inode mode/权限语义；保留参数 */

    proc_t *proc = Schedule::this_proc();
    if (!proc || !proc->pagemap) return -EINVAL;
    if (!is_user_address(path)) return -EFAULT;

    char *kpath = (char *)kmalloc(PATH_MAX);
    if (!kpath) return -ENOMEM;

    int64_t path_len = strncpy_from_user(kpath, (const char *)path, PATH_MAX,
                                         proc->pagemap);
    if (path_len <= 0) {          /* -EFAULT/-ENAMETOOLONG 或空路径 */
        kfree(kpath);
        return path_len < 0 ? (uint64_t)path_len : (uint64_t)(int64_t)-EINVAL;
    }

    __hmap_s_mp *MP = GetMount(kpath);
    if (!MP) {
        kfree(kpath);
        return -ENOENT;
    }
    if (MP->FST != FSType::FS_EXT4) {
        kfree(kpath);
        return -ENOSYS;
    }

    int32_t r = ext4_dir_mk(kpath);
    kfree(kpath);
    /* lwext4: 0 = EOK, 正数 = errno；syscall ABI 统一负 errno */
    return (uint64_t)(int64_t)(r > 0 ? -r : r);
}

/* ============================================================
 * sys_readdir —— 固定 syscall 23（ABI: path, buf, cap, flags）
 * ------------------------------------------------------------
 * 目录枚举（Explorer 文件浏览器用）。用户态给出路径 + 输出缓冲，内核
 * 一次性写入以 '\n' 分隔的**文本行**并追加结尾 '\0'：
 *
 *     "d|<name>\n"   目录
 *     "f|<name>\n"   普通文件
 *     "?|<name>\n"   其它类型（符号链接/设备等）
 *
 * 为什么用文本而不是二进制 struct：用户态与内核不必共享结构体布局，
 * 旧用户程序也不会因为结构体大小漂移读错内存；名称就是 ext4 目录项的
 * 原始字节（不含路径），最长 255 字节。'.' / '..' 与 inode==0 的空洞项
 * 由内核过滤，返回值为写入的**字节数**（不含结尾 NUL）。
 *
 *   - 只支持 ext4 挂载：其它挂载点返回 -ENOSYS（诚实语义）；
 *   - 缓冲不足时截断到**完整行**（绝不写半行），仍返回已写字节数；
 *   - cap 上限 64 KiB，避免用户态诱导内核做超大分配；
 *   - 返回负 errno：-EINVAL/-EFAULT/-ENOENT/-ENOMEM/-ENOSYS/-EIO。
 * 与 sys_mkdir 一样，目录迭代内部走 lwext4 的挂载点锁，此处不额外持锁。
 * ============================================================ */
uint64_t sys_readdir(uint64_t path, uint64_t buf, uint64_t cap,
                     GENERATE_IGN3()) {
    IGNV_3();
    proc_t *proc = Schedule::this_proc();
    if (!proc || !proc->pagemap) return -EINVAL;
    if (cap == 0u || cap > 65536u) return -EINVAL;
    if (!is_user_address(path) || !is_user_address(buf)) return -EFAULT;

    uint64_t user_space_end = IsPM5LVL ? USER_SPACE_END_5LVL : USER_SPACE_END_4LVL;
    if (buf >= user_space_end || cap > user_space_end - buf) return -EFAULT;

    char *kpath = (char *)kmalloc(PATH_MAX);
    if (!kpath) return -ENOMEM;
    int64_t path_len = strncpy_from_user(kpath, (const char *)path, PATH_MAX,
                                         proc->pagemap);
    if (path_len <= 0) {
        kfree(kpath);
        return path_len < 0 ? (uint64_t)path_len : (uint64_t)(int64_t)-EINVAL;
    }

    /* GetMountPointName() 要求路径形如 "/mp/<something>"（含第二个 '/'），
       而文件浏览器从挂载点根 "/mp" 开始枚举：直接查表会得到 nullptr ->
       -ENOENT。给纯挂载点路径补一个尾斜杠，归一化为 "/mp/"：既能命中挂载
       表（key 本来就是 "/mp/"），lwext4 也能把它当作根目录打开。
       带子路径的 "/mp/system" 保持不变。 */
    {
        const size_t plen = (size_t)path_len;
        bool has_second_slash = false;
        for (size_t i = 1; i < plen; i++)
            if (kpath[i] == '/') { has_second_slash = true; break; }
        if (!has_second_slash && plen + 1u < (size_t)PATH_MAX) {
            kpath[plen] = '/';
            kpath[plen + 1u] = '\0';
        }
    }

    __hmap_s_mp *MP = GetMount(kpath);
    if (!MP) { kfree(kpath); return -ENOENT; }
    if (MP->FST != FSType::FS_EXT4) { kfree(kpath); return -ENOSYS; }

    char *out = (char *)kmalloc(cap);
    if (!out) { kfree(kpath); return -ENOMEM; }
    _memset(out, 0, cap);

    ext4_dir dir;
    _memset(&dir, 0, sizeof(dir));
    int32_t r = ext4_dir_open(&dir, kpath);
    if (r != 0) {
        kfree(kpath);
        kfree(out);
        /* lwext4: 0 = EOK, 正数 = errno；syscall ABI 统一负 errno */
        return (uint64_t)(int64_t)-r;
    }

    size_t used = 0;
    for (;;) {
        const ext4_direntry *de = ext4_dir_entry_next(&dir);
        if (!de) break;
        if (de->inode == 0 || de->name_length == 0) continue;
        /* ext4_dir_entry_next() 先把 de->name 清零再拷 name_length 字节,
           所以这里一定是 NUL 结尾；仍按 255 上限兜底。 */
        const char *name = (const char *)de->name;
        if (name[0] == '.' && (name[1] == '\0' ||
                               (name[1] == '.' && name[2] == '\0')))
            continue;                       /* 过滤 . / .. */
        const char type = (de->inode_type == EXT4_DE_DIR) ? 'd'
                        : (de->inode_type == EXT4_DE_REG_FILE) ? 'f' : '?';
        size_t nlen = 0;
        while (nlen < (size_t)de->name_length && nlen < 255u && name[nlen]) nlen++;
        /* 每行 = type + '|' + name + '\n'，另留 1 字节给结尾 NUL */
        if (used + nlen + 3u >= cap) break;
        out[used++] = type;
        out[used++] = '|';
        for (size_t i = 0; i < nlen; i++) out[used++] = name[i];
        out[used++] = '\n';
    }
    out[used] = '\0';
    ext4_dir_close(&dir);
    kfree(kpath);

    if (!VMM::UserAccess::CopyToUser(proc->pagemap, buf, out, used + 1u)) {
        kfree(out);
        return -EFAULT;
    }
    kfree(out);
    return (uint64_t)used;
}