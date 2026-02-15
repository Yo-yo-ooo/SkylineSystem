#pragma once
#ifndef _AARCH64_VMM_H
#define _AARCH64_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <arch/aarch64/vmm/plvldescs.h>

typedef struct _pagemap_t{
    uint32_t   levels;
    void *top_level[2];
}pagemap_t;

enum page_size {
    Size4KiB,
    Size2MiB,
    Size1GiB
};


namespace VMM
{
    void Init();
    uint64_t GetPhysics(pagemap_t *pagemap, uint64_t vaddr);
} // namespace VMM


#endif