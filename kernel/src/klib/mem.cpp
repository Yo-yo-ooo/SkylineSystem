/*
* SPDX-License-Identifier: GPL-2.0-only
* File: mem.cpp
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
#include <klib/klib.h>
#include <klib/kio.h>
#include <conf.h>

/* #ifdef __x86_64__
#pragma GCC push_options
#endif
#if defined(__x86_64__) && NOT_COMPILE_X86MEM == 0
#include "../../../x86mem/x86mem.h"
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/sched.h>
#include <klib/serial.h>

#pragma GCC target("sse2")
#include <emmintrin.h>
#ifdef __AVX512F__
#pragma GCC target("avx512f")
#elif defined(__AVX2__)
#pragma GCC target("avx2")
#endif
#elif defined(__x86_64__)
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/sched.h>
#elif defined(__aarch64__)
func_optimize(3) void NEON_MEMCPY(void* dst, const void* src, size_t size);
func_optimize(3) void NEON_MEMSET(void* dst, uint8_t value, size_t size);
#endif

#ifdef __x86_64__
//X/FXSave Check And Set
/*
asm volatile("" ::: "memory");
    /\
    ||
    \/
Prevent the compiler from moving the code from the back to the front
*//* 
#define XFXSAVE_CAS do{\
        cpu->preempt_count++; \    
        asm volatile("" ::: "memory"); \ 
        if(cpu->SupportXSAVE){ \
            if(fx_area != nullptr){ \
                if(cpu->SupportXSAVEOPT) \
                    asm volatile("xsaveopt %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");\
                else if(cpu->SupportXSAVE)\
                    asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
            } \
            asm volatile("xrstor %0" : : "m"(*cpu->KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
        }else{ \
            if(fx_area != nullptr) \
                asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory"); \
            asm volatile("fxrstor (%0)" : : "r"(cpu->KernelXsaveSpace) : "memory"); \
        } \
    }while(0);
 */
//X/FXSave Check And Set Back
/*
asm volatile("" ::: "memory");
    /\
    ||
    \/
Prevent the compiler from moving previous code to the end
*/
/* #define XFXSAVE_CASB do{\
        if(cpu->SupportXSAVE){ \
            if(cpu->SupportXSAVEOPT) \
                asm volatile("xsaveopt %0" : : "m"(*cpu->KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
            else \
                asm volatile("xsave %0" : : "m"(*cpu->KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
            if(fx_area != nullptr) \
                asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory"); \
        }else{ \
            asm volatile("fxsave (%0)" : : "r"(cpu->KernelXsaveSpace) : "memory"); \
            if(fx_area != nullptr) \
                asm volatile("fxrstor (%0)" : : "r"(fx_area) : "memory"); \
        } \
        asm volatile("" ::: "memory"); \ 
        cpu->preempt_count--; \
    }while(0);

#endif */ 

extern "C" {
void _memcpy(void* src, void* dest, uint64_t size){
/* #if defined(__x86_64__)
#if (defined(COMPILER_SUPPORT_AVX512) || \
    defined(COMPILER_SUPPORT_AVX2) || \
    defined(COMPILER_SUPPORT_SSE_4_2)) && (USE_HOST_CPU_EXTENSIONS == 1)
    cpu_t *cpu = this_cpu();
    if(cpu == nullptr || (cpu->InIntr == true))
        goto base_ver;
    if(size >= 79872 78K && cpu->SupportSSE4_2){
        int8_t *fx_area = Schedule::this_thread()->fx_area;
        if(fx_area == nullptr)
            goto base_ver;
        XFXSAVE_CAS
        AVX_memcpy(dest,src,size);
        XFXSAVE_CASB
        return;
    }
#endif
#else defined(__aarch64__)
    NEON_MEMCPY(dest,src,size);
    return;
#endif 
base_ver: */
    memcpy_fscpuf(dest,src,size);
}


void _memset(void* dest, uint8_t value, uint64_t size){
/* #if defined(__x86_64__)
#if (defined(COMPILER_SUPPORT_AVX512) || \
    defined(COMPILER_SUPPORT_AVX2) || \
    defined(COMPILER_SUPPORT_SSE_4_2)) && (USE_HOST_CPU_EXTENSIONS == 1)
    cpu_t *cpu = this_cpu();
    if(cpu == nullptr || (cpu->InIntr == true))
        goto base_ver;
    if(size >= 32768 && cpu->SupportSSE4_2){
        
        int8_t *fx_area = Schedule::this_thread()->fx_area;
        if(fx_area == nullptr)
            goto base_ver;
        XFXSAVE_CAS
        AVX_memset(dest,value,size);
        XFXSAVE_CASB
        return;
    }
#endif
#else defined(__aarch64__)
    NEON_MEMSET(dest,value,size);
    return;
#endif
base_ver: */
    memset_fscpuf(dest,(int32_t)value,size);
}


void _memmove(void* dest,void* src, uint64_t size) {
/* #if defined(__x86_64__)
#if (defined(COMPILER_SUPPORT_AVX512) || \
    defined(COMPILER_SUPPORT_AVX2) || \
    defined(COMPILER_SUPPORT_SSE_4_2)) && (USE_HOST_CPU_EXTENSIONS == 1)
    cpu_t *cpu = this_cpu();
    if(cpu == nullptr || (cpu->InIntr == true))
        goto base_ver;
    if(size >= 131072 && cpu->SupportSSE4_2){
        int8_t *fx_area = Schedule::this_thread()->fx_area;
        if(fx_area == nullptr)
            goto base_ver;
        XFXSAVE_CAS
        AVX_memmove(dest,src,size);
        XFXSAVE_CASB
        return;
    }
#endif
#endif
base_ver: */
    memmove_fscpuf(dest,src,size);
}

int32_t _memcmp(const void* buffer1,const void* buffer2,size_t  size){
/* #if defined(__x86_64__)
#if (defined(COMPILER_SUPPORT_AVX512) || \
    defined(COMPILER_SUPPORT_AVX2) || \
    defined(COMPILER_SUPPORT_SSE_4_2)) && (USE_HOST_CPU_EXTENSIONS == 1)
    cpu_t *cpu = this_cpu();
    if(cpu == nullptr || (cpu->InIntr == true))
        goto base_ver;
    if(size >= 1024 && cpu->SupportSSE4_2){
        int8_t *fx_area = Schedule::this_thread()->fx_area;
        if(fx_area == nullptr)
            goto base_ver;
        XFXSAVE_CAS
        int32_t result = AVX_memcmp(buffer1,buffer2,size,1);
        XFXSAVE_CASB
        return result;
    }
#endif
#endif
base_ver: */
    return memcmp_fscpuf(buffer1,buffer2,size);
}
}

#ifdef __x86_64__
#pragma GCC pop_options
#endif