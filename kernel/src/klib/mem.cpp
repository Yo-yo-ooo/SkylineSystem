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
#include <emmintrin.h>

typedef char xmm_t __attribute__((__vector_size__(16), __aligned__(1)));
#ifdef __AVX512F__
#pragma GCC target("avx512f")
#elif defined(__AVX2__)
#pragma GCC target("avx2")
#endif
#elif defined(__x86_64__)
#include <arch/x86_64/smp/smp.h>
#include <arch/x86_64/schedule/sched.h>
#elif defined(__aarch64__)
extern "C" void NEON_MEMCPY(void* dst, const void* src, size_t size);
extern "C" void NEON_MEMSET(void* dst, unsigned char value, size_t size);
#endif


void _memcpy_128(void* src, void* dest, size_t size)
{
	auto _src = (__uint128_t*)src;
	auto _dest = (__uint128_t*)dest;
	size >>= 4; // size /= 16
	while (size--)
		*(_dest++) = *(_src++);
}

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
#elif defined(__x86_64__)

    
#elif defined(__aarch64__)
    NEON_MEMCPY(dest,src,size);
    if(KernelInited == false)
        return;
#endif

#ifdef __x86_64__
    if(((KernelInited == false) || (size > 1024 * 8))){
        /// We will use pointer arithmetic, so char pointer will be used.
        /// Note that __restrict makes sense (otherwise compiler will reload data from memory
        /// instead of using the value of registers due to possible aliasing).
        char* __restrict dst = reinterpret_cast<char* __restrict>(dest);
        const char* __restrict src_ = reinterpret_cast<const char* __restrict>(src);

        /// Standard memcpy returns the original value of dst. It is rarely used but we have to do it.
        /// If you use memcpy with small but non-constant sizes, you can call inline_memcpy directly
        /// for inlining and removing this single instruction.
        void* ret = dst;

    tail:
        /// Small sizes and tails after the loop for large sizes.
        /// The order of branches is important but in fact the optimal order depends on the distribution of sizes in your application.
        /// This order of branches is from the disassembly of glibc's code.
        /// We copy chunks of possibly uneven size with two overlapping movs.
        /// Example: to copy 5 bytes [0, 1, 2, 3, 4] we will copy tail [1, 2, 3, 4] first and then head [0, 1, 2, 3].
        if (size <= 16)
        {
            if (size >= 8)
            {
                /// Chunks of 8..16 bytes.
                memcpy_fscpuf(dst + size - 8, src_ + size - 8, 8);
                memcpy_fscpuf(dst, src_, 8);
            }
            else if (size >= 4)
            {
                /// Chunks of 4..7 bytes.
                memcpy_fscpuf(dst + size - 4, src_ + size - 4, 4);
                memcpy_fscpuf(dst, src_, 4);
            }
            else if (size >= 2)
            {
                /// Chunks of 2..3 bytes.
                memcpy_fscpuf(dst + size - 2, src_ + size - 2, 2);
                memcpy_fscpuf(dst, src_, 2);
            }
            else if (size >= 1)
            {
                /// A single byte.
                *dst = *src_;
            }
            /// No bytes remaining.
        }
        else
        {
            /// Medium and large sizes.
            if (size <= 128)
            {
                /// Medium size, not enough for full loop unrolling.

                /// We will copy the last 16 bytes.
                _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + size - 16), _mm_loadu_si128(reinterpret_cast<const __m128i *>(src_ + size - 16)));

                /// Then we will copy every 16 bytes from the beginning in a loop.
                /// The last loop iteration will possibly overwrite some part of already copied last 16 bytes.
                /// This is Ok, similar to the code for small sizes above.
                while (size > 16)
                {
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), _mm_loadu_si128(reinterpret_cast<const __m128i *>(src_)));
                    dst += 16;
                    src_ += 16;
                    size -= 16;
                }
            }
            else
            {
                /// Large size with fully unrolled loop.

                /// Align destination to 16 bytes boundary.
                size_t padding = (16 - (reinterpret_cast<size_t>(dst) & 15)) & 15;

                /// If not aligned - we will copy first 16 bytes with unaligned stores.
                if (padding > 0)
                {
                    __m128i head = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), head);
                    dst += padding;
                    src_ += padding;
                    size -= padding;
                }

                /// Aligned unrolled copy. We will use half of available SSE registers.
                /// It's not possible to have both src_ and dst aligned.
                /// So, we will use aligned stores and unaligned loads.
                __m128i c0, c1, c2, c3, c4, c5, c6, c7;

                while (size >= 128)
                {
                    c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_) + 0);
                    c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_) + 1);
                    c2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_) + 2);
                    c3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_) + 3);
                    c4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_) + 4);
                    c5 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_) + 5);
                    c6 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_) + 6);
                    c7 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src_) + 7);
                    src_ += 128;
                    _mm_store_si128((reinterpret_cast<__m128i*>(dst) + 0), c0);
                    _mm_store_si128((reinterpret_cast<__m128i*>(dst) + 1), c1);
                    _mm_store_si128((reinterpret_cast<__m128i*>(dst) + 2), c2);
                    _mm_store_si128((reinterpret_cast<__m128i*>(dst) + 3), c3);
                    _mm_store_si128((reinterpret_cast<__m128i*>(dst) + 4), c4);
                    _mm_store_si128((reinterpret_cast<__m128i*>(dst) + 5), c5);
                    _mm_store_si128((reinterpret_cast<__m128i*>(dst) + 6), c6);
                    _mm_store_si128((reinterpret_cast<__m128i*>(dst) + 7), c7);
                    dst += 128;

                    size -= 128;
                }

                /// The latest remaining 0..127 bytes will be processed as usual.
                goto tail;
            }
        }
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
#elif defined(__x86_64__) // For General x86_64 cpu(DIDn't support >=sse4.2)
    if(/* smp_started != false &&  */((KernelInited == false) || (size > 1024 * 8))){
        uint64_t Loop128C = size / 128;
        __m128i val = reinterpret_cast<__m128i>(_mm_set1_epi8((char)value));
        for(uint64_t i = 0; i < Loop128C; i++){
            _mm_store_si128(reinterpret_cast<__m128i*>((uint64_t)dest + i * 16), val);
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

#elif defined(__aarch64__)
    NEON_MEMSET(dest,value,size);
    return;
#endif
#ifdef __x86_64__
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
        return;
    }

    if(KernelInited && size > 1024 * 8){
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
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMCMP) && NOT_COMPILE_X86MEM == 0
    cpu_t *cpu = this_cpu();
    int8_t *fx_area = nullptr;
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
    if(smp_started != false && cpu->SupportSSE4_2 && ((KernelInited == false) || (count > 1024 * 8))){
        return AVX_memcmp(buffer1,buffer2,count,1);
    }
    if(KernelInited && count > 1024 * 8){
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
    return memcmp_fscpuf(buffer1,buffer2,count);
}

#ifdef __x86_64__
#pragma GCC pop_options
#endif