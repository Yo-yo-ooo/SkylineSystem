//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only

#include <klib/kio.h>
#include <arch/x86_64/smp/smp.h>

void WRFSBASE_V0(uint64_t value){
    wrmsr(FS_BASE, value);
}

void WRFSBASE_V1(uint64_t value){
    asm volatile("wrfsbase %0" : : "r" (value) : "memory");
}

