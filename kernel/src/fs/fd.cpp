/*
* SPDX-License-Identifier: GPL-2.0-only
* File: fd.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/
#include <fs/fd.h>
#include <fs/lwext4/ext4.h>
#include <klib/algorithm/hmap.h>



#include <klib/klib.h>
#include <mem/heap.h>

extern "C" char* GetMountPointName(const char* path) {
    if (!path || path[0] != '/') return nullptr;

    // 找到第一个有效字符后的第一个 '/'
    // 比如 "/mp/test" -> 找到 'p' 后面的那个 '/'
    const char* p = path + 1;
    while (*p && *p == '/') p++; // 跳过开头重复的斜杠，如 "//mp" -> 指向 "mp"
    
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




extern "C" int32_t __hmap_s_mp_compare(const void* a,const void* b){
    return strcmp(((struct __hmap_s_mp*)a)->MPName,((struct __hmap_s_mp*)b)->MPName);
}

extern "C" uint64_t __hmap_s_mp_hash(const void* item, uint64_t seed0, uint64_t seed1){
    const struct __hmap_s_mp* entry = (const struct __hmap_s_mp*)item;
    return hashmap_sip(entry->MPName, strlen(entry->MPName), seed0, seed1);
}

extern "C" volatile struct hashmap* HMapS_MP = nullptr;

