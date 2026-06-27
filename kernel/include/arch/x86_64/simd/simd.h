//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
#pragma once
#ifndef x86MEM_ENABLE_H
#define x86MEM_ENABLE_H

#include <stdint.h>
#include <stddef.h>
#include <klib/kio.h>

extern "C++" {

#ifdef __x86_64__


void simd_cpu_init(cpu_t *cpu);

void StoreSIMDState_V0(char* area,uint32_t Lo,uint32_t Hi);
void StoreSIMDState_V1(char* area,uint32_t Lo,uint32_t Hi);
void StoreSIMDState_V2(char* area,uint32_t Lo,uint32_t Hi);
void LoadSIMDState_V0(char* area,uint32_t Lo,uint32_t Hi);
void LoadSIMDState_V1(char* area,uint32_t Lo,uint32_t Hi);

#else
#error "You include x86_64 ARCH file,but your ARCH not x86_64"
#endif

}

#endif