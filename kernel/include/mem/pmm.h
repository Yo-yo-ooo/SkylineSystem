//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <klib/types.h>

namespace PMM{

    extern volatile uint8_t *bitmap;
    // 升级为三级位图：L2 (2MB块), L3 (2GB块)
    extern volatile uint64_t *bitmap_l2;
    extern volatile uint64_t *bitmap_l3;

    extern volatile uint64_t bitmap_size;
    // 同步更新大小变量
    extern volatile uint64_t bitmap_l2_size;
    extern volatile uint64_t bitmap_l3_size;
    extern volatile uint64_t bitmap_last_free;

    extern volatile uint64_t pmm_bitmap_start;
    extern volatile uint64_t pmm_bitmap_size;
    extern volatile uint64_t pmm_bitmap_pages;

    void bitmap_clear_(uint64_t bit);
    void bitmap_set_(uint64_t bit);
    bool bitmap_test_(uint64_t bit);
#ifdef __x86_64__
    void* GlobalRequestSingle();
#endif

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