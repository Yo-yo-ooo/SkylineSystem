#pragma once

#include <klib/types.h>

namespace PMM{

    extern volatile uint8_t *bitmap;
    extern volatile uint64_t bitmap_size;
    extern volatile uint64_t bitmap_last_free;

    extern volatile uint64_t pmm_bitmap_start;
    extern volatile uint64_t pmm_bitmap_size;
    extern volatile uint64_t pmm_bitmap_pages;
    void bitmap_clear_(uint64_t bit);
    void bitmap_set_(uint64_t bit);
    bool bitmap_test_(uint64_t bit);

    void Init();
    
    void *Request();
    uint64_t Request_();
    void Free(void *ptr);
}