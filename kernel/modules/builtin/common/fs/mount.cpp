//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
// SPDX-License-Identifier: GPL-2.0-only
/* mount.cpp —— 挂载点表实现，从 fd.cpp 拆出并随 kernel.dev.core.ko 迁移。
 * fd.cpp 保留 per-process fd_manager（kernel task.cpp 直接调用）。 */
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

__hmap_s_mp *GetMountImpl(const char *path) {
    spinlock_lock(&hmap_mp_lock);
    __hmap_s_mp *ret = GetMount_NoLock(path);
    spinlock_unlock(&hmap_mp_lock);
    return ret;
}
