//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#ifndef _AARCH64_VMM_H
#define _AARCH64_VMM_H

#include <stdint.h>
#include <stddef.h>
#include <arch/aarch64/vmm/plvldescs.h>

typedef struct _pagemap_t{
    uint32_t   levels;
    void *top_level[2]; // 0:LowerRoot,1:HigherRoot!
}pagemap_t;

enum page_size {
    Size4KiB,
    Size2MiB,
    Size1GiB
};

#define UXN (1ULL << 54) //User mode cannot Run
#define PXN (1ULL << 53) //Kernel mode cnnot run(data)

namespace VMM
{
    extern volatile pagemap_t *KernelPageMap;
    void Init();
    void SwitchPM(pagemap_t *pagemap);
} // namespace VMM


#endif