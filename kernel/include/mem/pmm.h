//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <klib/types.h>

namespace PMM{
    extern uint64_t pmm_bitmap_pages;
    extern uint64_t pmm_bitmap_start;
    extern uint64_t pmm_bitmap_size;

    uint64_t FreePages();
    void dbg_dump();   // [DBG] temporary per-pid pool-holdings dump
    void dbg_snapshot(const char* why); // [DBG] manual snapshot
    void dbg_mcharge(uint64_t pid, int64_t pages); // [DBG] mmap layer
    
    void Init();
    void* Request(uint64_t n = 1);
    void Free(void *ptr, uint64_t n = 1);

    // 新增 2MB 大页分配与释放
    void* Request2MB();
    void Free2MB(void *ptr);

    // 新增 2GB 大页分配与释放
    void* Request2GB();
    void Free2GB(void *ptr);
}