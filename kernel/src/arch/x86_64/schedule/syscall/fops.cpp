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
    
    // 获取当前文件偏移量，解决缓存未包含偏移导致的数据错位 Bug
    uint64_t cur_offset = FD->FSOPS->lseek(FD->filedesc, 0, SEEK_CUR);
    
    void *cached_data = file_cache_get(cpu->file_cache, FD->path, FD->path_len, count, &out_len, &cache_entry);
    
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
            file_cache_record_io(cpu->file_cache, FD->path, FD->path_len, count, NULL, FD->file_size, (uint64_t)FD->filedesc);
            return count;
        }
        // 读取范围超出缓存范围，放弃缓存，回退到硬件读取
        file_cache_put(cpu->file_cache, cache_entry);
    }

    // Cache Miss: Hardware
    void* kbuf = kmalloc(count);
    if (!kbuf) return -ENOMEM;
    
    // 确保文件偏移量正确（前面 lseek 可能未改变，但保险起见恢复状态）
    FD->FSOPS->lseek(FD->filedesc, cur_offset, SEEK_SET);

    size_t total_read = 0;
    FD->FSOPS->read(FD->filedesc, kbuf, count, &total_read);
    
    if (total_read > 0) {
        
        if (!VMM::UserAccess::CopyToUser(proc->pagemap, buf, kbuf, total_read)) {
            kfree(kbuf);
            return -EFAULT;
        }

        // 只有从 offset 0 开始读取的数据，才能安全作为整个文件的缓存前缀进行记录/提升
        if (cur_offset == 0) {
            void *cache_buf = kmalloc(total_read);
            if (cache_buf) {
                __memcpy(cache_buf, kbuf, total_read);
                file_cache_record_io(cpu->file_cache, FD->path, FD->path_len, total_read, cache_buf, FD->file_size, (uint64_t)FD->filedesc);
            }
        } else {
            // 非零偏移读取，不进行 promote，但仍记录 IO 统计以供启发式策略使用
            file_cache_record_io(cpu->file_cache, FD->path, FD->path_len, total_read, NULL, FD->file_size, (uint64_t)FD->filedesc);
        }
    }

    kfree(kbuf);
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

    // 获取写入前的偏移量
    uint64_t cur_offset = FD->FSOPS->lseek(FD->filedesc, 0, SEEK_CUR);

    size_t wcnt = 0;
    int32_t status = FD->FSOPS->write(FD->filedesc, kbuf, count, &wcnt);
    
    if (status == 0 && wcnt > 0) {
        cpu_t *cpu = this_cpu();
        size_t out_len = 0;
        file_cache_entry_t *cache_entry = NULL;
        
        // 尝试获取缓存
        void *cached_data = file_cache_get(cpu->file_cache, FD->path, FD->path_len, wcnt, &out_len, &cache_entry);
        
        if (cached_data) {
            // 缓存命中修改逻辑：如果写入的范围完全在缓存覆盖范围内
            if (cur_offset + wcnt <= out_len) {
                // 直接在内存中修改缓存对应位置的数据 (Write-through 策略)
                __memcpy((void*)((uint64_t)cached_data + cur_offset), kbuf, wcnt);
                // 因为数据已经同步写入磁盘了，所以不需要标记脏页，缓存与磁盘保持一致
            } else {
                // 写入超出了缓存范围，现有缓存不再能代表文件前缀，使其失效
                file_cache_put(cpu->file_cache, cache_entry);
                file_cache_invalidate(cpu->file_cache, FD->path, FD->path_len);
                cache_entry = NULL;
            }
            if (cache_entry) {
                file_cache_put(cpu->file_cache, cache_entry);
            }
            file_cache_record_io(cpu->file_cache, FD->path, FD->path_len, wcnt, NULL, FD->file_size, (uint64_t)FD->filedesc);
        } else {
            // 缓存未命中。如果是从 offset 0 开始写，则可以将这批数据作为新的缓存块 promote
            if (cur_offset == 0) {
                void *cache_buf = kmalloc(wcnt);
                if (cache_buf) {
                    __memcpy(cache_buf, kbuf, wcnt);
                    // is_dirty = false，因为 FSOPS->write 已经同步落盘
                    int32_t r = file_cache_promote(cpu->file_cache, FD->path, FD->path_len, cache_buf, wcnt, false, FD->file_size, (uint64_t)FD->filedesc);
                    if (r != 0) kfree(cache_buf); 
                }
            } else {
                file_cache_record_io(cpu->file_cache, FD->path, FD->path_len, wcnt, NULL, FD->file_size, (uint64_t)FD->filedesc);
            }
        }
        
        // 修复 Bug 5: 不能盲目 +=，必须从文件系统获取真实大小
        FD->file_size = FD->FSOPS->fsize(FD->filedesc);
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
    return FD->FSOPS->lseek(FD->filedesc,offset,whence);
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
    fd_struct->filedesc = kmalloc(MP->FSOPS->SIZEOF_FILE_DESC);
    _memset(fd_struct->filedesc,0,MP->FSOPS->SIZEOF_FILE_DESC);
    
    int32_t err = MP->FSOPS->open(fd_struct->filedesc, kpath, flags);

    if (err < 0) {
        kfree(fd_struct->filedesc);
        fd_free(proc->FDMan, fd_idx);
        kfree(kpath);
        return err; 
    }

    // 修复 Bug 6: 额外分配1字节用于 NUL 终止符，防止越界读
    fd_struct->path = (uint8_t*)kmalloc(path_len + 1);
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
    int32_t res = FD->FSOPS->close(FD->filedesc);
    
    if (FD->path) {
        kfree(FD->path);
        FD->path = NULL;
    }

    fd_free(proc->FDMan,fd);
    return res;
}

uint64_t sys_mkdir(uint64_t path,uint64_t mode,GENERATE_IGN4()){
    IGNV_4();
    return -ENOSYS; // Function not implemented
}