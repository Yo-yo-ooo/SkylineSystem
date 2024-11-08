#pragma once

#include "../klib/klib.h"

namespace PMM{
    void Init();

    u64 FindPages(usize n);
    void* Alloc(usize n);
    void Free(void* ptr, usize n);
}

#define pmm_find_pages PMM::FindPages
#define pmm_alloc PMM::Alloc
#define pmm_free PMM::Free