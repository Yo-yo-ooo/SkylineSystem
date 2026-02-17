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
#if defined(__x86_64__) && NOT_COMPILE_X86MEM == 0
#include "../../../x86mem/x86mem.h"
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/sched.h>
#include <klib/serial.h>
#pragma GCC target("sse2")
#ifdef __AVX512F__
#pragma GCC target("avx512f,avx512bw,avx512dq,avx512vl")
#elif defined(__AVX2__)
#pragma GCC target("avx2")
#endif
#elif defined(__x86_64__)
#pragma GCC target("sse2")
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/sched.h>
#include <emmintrin.h>
#elif defined(__aarch64__)
extern "C" void NEON_MEMCPY(void* dst, const void* src, size_t size);
extern "C" void NEON_MEMSET(void* dst, unsigned char value, size_t size);
#endif

#ifdef __x86_64__
extern "C" int32_t __sse_memcmp(const void *a, const void *b, size_t size);
#endif


void _memcpy(void* src, void* dest, uint64_t size)
{
#ifdef __x86_64__
    cpu_t *cpu = this_cpu();
    int8_t *fx_area = nullptr;
    thread_t *th = Schedule::this_thread();
    if(th != nullptr)
        fx_area = th->fx_area;
    if(KernelInited && size > 1024 * 8){
        if(cpu->SupportXSAVE){
            if(fx_area != nullptr)
                asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            if(fx_area != nullptr)
                asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(KernelXsaveSpace) : "memory");
        }
    }
#endif


#if defined(__x86_64__) && defined(CONFIG_FAST_MEMCPY) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && cpu->SupportSSE4_2 && ((KernelInited == false) || (size > 1024 * 8))){
        AVX_memcpy(dest,src,size);
        if(KernelInited == false)
            return;
        goto end_deal;
    }
#elif defined(__aarch64__)
    NEON_MEMCPY(dest,src,size);
    if(KernelInited == false)
        return;
#endif

#ifdef __x86_64__
    if(/* smp_started != false &&  */((KernelInited == false) || (size > 1024 * 8))){
        __m128i *srcPtr = (__m128i *)src;
        __m128i *destPtr = (__m128i *)dest;

        size_t index = 0;
        while(size) {
            __m128i x = _mm_load_si128(&srcPtr[index]);
            _mm_stream_si128(&destPtr[index], x);

            size -= 16;
            index++;
        }
        memcpy_fscpuf(dest,src,size);
        if(KernelInited == false)
            return;
        goto end_deal;
    }
    if(KernelInited && size > 1024 * 8){
    end_deal:
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            if(fx_area != nullptr)
                asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(KernelXsaveSpace) : "memory");
            if(fx_area != nullptr)
                asm volatile("fxrstor (%0)" : : "r"(fx_area) : "memory");
        }
        return;
    }
#endif
	memcpy_fscpuf(dest,src,size);
}


void _memset(void* dest, uint8_t value, uint64_t size)
{
#ifdef __x86_64__
    cpu_t *cpu = this_cpu();
    int8_t *fx_area = nullptr;
    thread_t *th = Schedule::this_thread();
    if(th != nullptr)
        fx_area = th->fx_area;
    
    if(KernelInited && size > 1024 * 8){
        if(cpu->SupportXSAVE){
            if(fx_area != nullptr)
                asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            if(fx_area != nullptr)
                asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(KernelXsaveSpace) : "memory");
        }
    }
#endif


#if defined(__x86_64__) && defined(CONFIG_FAST_MEMSET) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && cpu->SupportSSE4_2 && ((KernelInited == false) || (size > 1024 * 8))){
        AVX_memset(dest,value,size);
        if(KernelInited == false)
            return;
        goto end_deal;
    }
#elif defined(__aarch64__)
    NEON_MEMSET(dest,value,size);
    return;
#endif
#ifdef __x86_64__
    // For General x86_64 cpu(DIDn't support >=sse4.2)
    if(/* smp_started != false &&  */((KernelInited == false) || (size > 1024 * 8))){
        uint64_t Loop128C = size / 128;
        __m128i val = _mm_set1_epi8((char)value);
        for(uint64_t i = 0; i < Loop128C; i++){
            _mm_store_si128((__m128i*)((uint64_t)dest + i * 16), val);
            size -= 16;
        }
        /* if(size != 0){
            char* d = (char*)dest;
            for (uint64_t i = 0; i < size; i++)
                *(d++) = value;
        }
        //return; */
        memset_fscpuf(dest,(const int32_t)value,size);
        if(KernelInited == false)
            return;
        goto end_deal;
    }
    if(KernelInited && size > 1024 * 8){
    end_deal:
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            if(fx_area != nullptr)
                asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(KernelXsaveSpace) : "memory");
            if(fx_area != nullptr)
                asm volatile("fxrstor (%0)" : : "r"(fx_area) : "memory");
        }
        return;
    }
#endif

	memset_fscpuf(dest,(const int32_t)value,size);
}


void _memmove(void* dest,void* src, uint64_t size) {
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMMOVE) && NOT_COMPILE_X86MEM == 0
    cpu_t *cpu = this_cpu();
    int8_t *fx_area = nullptr;
    thread_t *th = Schedule::this_thread();
    if(th != nullptr)
        fx_area = th->fx_area;
    
    if(KernelInited && size > 1024 * 8){
        if(cpu->SupportXSAVE){
            if(fx_area != nullptr)
                asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            if(fx_area != nullptr)
                asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(KernelXsaveSpace) : "memory");
        }
    }
    if(smp_started != false && cpu->SupportSSE4_2 && ((KernelInited == false) || (size > 1024 * 8))){
        AVX_memmove(dest,src,size);
        if(KernelInited == false)
            return;
        goto end_deal;
    }

    if(KernelInited && size > 1024 * 8){
    end_deal:
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            if(fx_area != nullptr)
                asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(KernelXsaveSpace) : "memory");
            if(fx_area != nullptr)
                asm volatile("fxrstor (%0)" : : "r"(fx_area) : "memory");
        }
    }
#endif
	memmove_fscpuf(dest,src,size);
}

int32_t _memcmp(const void* buffer1,const void* buffer2,size_t  count)
{
#ifdef __x86_64__
    cpu_t *cpu = this_cpu();
    int8_t *fx_area = nullptr;
    int32_t ans;
    thread_t *th = Schedule::this_thread();
    if(th != nullptr)
        fx_area = th->fx_area;
    if(KernelInited && count > 1024 * 8){
        if(cpu->SupportXSAVE){
            if(fx_area != nullptr)
                asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            if(fx_area != nullptr)
                asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(KernelXsaveSpace) : "memory");
        }
    }
#endif

#if defined(__x86_64__) && defined(CONFIG_FAST_MEMCMP) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && cpu->SupportSSE4_2 && ((KernelInited == false) || (count > 1024 * 8))){
        ans = AVX_memcmp(buffer1,buffer2,count,1);
        if(KernelInited == false)
            return ans;
        goto end_deal;
    }
#endif

#ifdef __x86_64__
    if(((KernelInited == false) || (count > 1024 * 8))){
        ans = __sse_memcmp(buffer1,buffer2,count);
        if(KernelInited == false)
            return ans;
        goto end_deal;
    }
    if(KernelInited && count > 1024 * 8){
    end_deal:
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            if(fx_area != nullptr)
                asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(KernelXsaveSpace) : "memory");
            if(fx_area != nullptr)
                asm volatile("fxrstor (%0)" : : "r"(fx_area) : "memory");
        }
        return ans;
    }
#endif
    return memcmp_fscpuf(buffer1,buffer2,count);
}

#ifdef __x86_64__
#pragma GCC pop_options
#endif