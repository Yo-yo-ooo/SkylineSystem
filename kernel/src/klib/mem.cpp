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

void * __memcpy_fpx86 (void *dest, const void *src, size_t len)
{
    const char *s = (char*)src;
    char *d = (char*)dest;

    while (len--)
    {
        *d++ = *s++;
    }

    return dest;
}

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
            asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(KernelXsaveSpace) : "memory");
        }
    }
#endif


#if defined(__x86_64__) && defined(CONFIG_FAST_MEMCPY) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && cpu->SupportSSE4_2 && ((KernelInited == false) || (size > 1024 * 8))){
        AVX_memcpy(dest,src,size);
        return;
    }
#elif defined(__x86_64__)

    if(smp_started != false && ((KernelInited == false) || (size > 1024 * 8))){
        /// We will use pointer arithmetic, so char pointer will be used.
        /// Note that __restrict makes sense (otherwise compiler will reload data from memory
        /// instead of using the value of registers due to possible aliasing).
        char* __restrict dst = reinterpret_cast<char* __restrict>(dest);
        const char* __restrict src = reinterpret_cast<const char* __restrict>(src);

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
                __memcpy_fpx86(dst + size - 8, src + size - 8, 8);
                __memcpy_fpx86(dst, src, 8);
            }
            else if (size >= 4)
            {
                /// Chunks of 4..7 bytes.
                __memcpy_fpx86(dst + size - 4, src + size - 4, 4);
                __memcpy_fpx86(dst, src, 4);
            }
            else if (size >= 2)
            {
                /// Chunks of 2..3 bytes.
                __memcpy_fpx86(dst + size - 2, src + size - 2, 2);
                __memcpy_fpx86(dst, src, 2);
            }
            else if (size >= 1)
            {
                /// A single byte.
                *dst = *src;
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
                _mm_storeu_si128(reinterpret_cast<__m128i *>(dst + size - 16), _mm_loadu_si128(reinterpret_cast<const __m128i *>(src + size - 16)));

                /// Then we will copy every 16 bytes from the beginning in a loop.
                /// The last loop iteration will possibly overwrite some part of already copied last 16 bytes.
                /// This is Ok, similar to the code for small sizes above.
                while (size > 16)
                {
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), _mm_loadu_si128(reinterpret_cast<const __m128i *>(src)));
                    dst += 16;
                    src += 16;
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
                    __m128i head = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
                    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), head);
                    dst += padding;
                    src += padding;
                    size -= padding;
                }

                /// Aligned unrolled copy. We will use half of available SSE registers.
                /// It's not possible to have both src and dst aligned.
                /// So, we will use aligned stores and unaligned loads.
                __m128i c0, c1, c2, c3, c4, c5, c6, c7;

                while (size >= 128)
                {
                    c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 0);
                    c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 1);
                    c2 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 2);
                    c3 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 3);
                    c4 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 4);
                    c5 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 5);
                    c6 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 6);
                    c7 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src) + 7);
                    src += 128;
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

        return ret;
    }
#elif defined(__aarch64__)
    NEON_MEMCPY(dest,src,size);
    return;
#endif

#ifdef __x86_64__
    if(KernelInited && size > 1024 * 8){
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(KernelXsaveSpace) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(fx_area) : "memory");
        }
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
            asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(KernelXsaveSpace) : "memory");
        }
    }
#endif


#if defined(__x86_64__) && defined(CONFIG_FAST_MEMSET) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && cpu->SupportSSE4_2 && ((KernelInited == false) || (size > 1024 * 8))){
        AVX_memset(dest,value,size);
        return;
    }
#elif defined(__x86_64__) // For General x86_64 cpu(DIDn't support >=sse4.2)
    if(smp_started != false && ((KernelInited == false) || (size > 1024 * 8))){
        uint64_t Loop128C = size / 128;
        __m128i val = _mm_set1_epi8((char)value);
        for(uint64_t i = 0; i < Loop128C; i++){
            _mm_store_si128((__m128i*)((uint64_t)dest + i * 16), val);
            size -= 16;
        }
        if(size != 0){
            char* d = (char*)dest;
            for (uint64_t i = 0; i < size; i++)
                *(d++) = value;
        }
        return;
    }

#elif defined(__aarch64__)
    NEON_MEMSET(dest,value,size);
    return;
#endif
#ifdef __x86_64__
    if(KernelInited && size > 1024 * 8){
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(KernelXsaveSpace) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(fx_area) : "memory");
        }
    }
#endif

	memset_fscpuf(dest,(const int32_t)value,size);
}


void _memmove(void* dest,void* src, uint64_t size) {

#ifdef __x86_64__
    cpu_t *cpu = this_cpu();
    int8_t *fx_area = nullptr;
    thread_t *th = Schedule::this_thread();
    if(th != nullptr)
        fx_area = th->fx_area;
    
    if(KernelInited && size > 1024 * 8){
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(KernelXsaveSpace) : "memory");
        }
    }
#endif
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMMOVE) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && cpu->SupportSSE4_2 && ((KernelInited == false) || (size > 1024 * 8))){
        AVX_memmove(dest,src,size);
        return;
    }
#endif

#ifdef __x86_64__
    if(KernelInited && size > 1024 * 8){
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(KernelXsaveSpace) : "memory");
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
    thread_t *th = Schedule::this_thread();
    if(th != nullptr)
        fx_area = th->fx_area;
    
    if(KernelInited && count > 1024 * 8){
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(fx_area) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(KernelXsaveSpace) : "memory");
        }
    }
#endif

#if defined(__x86_64__) && defined(CONFIG_FAST_MEMCMP) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && cpu->SupportSSE4_2 && ((KernelInited == false) || (count > 1024 * 8))){
        return AVX_memcmp(buffer1,buffer2,count,1);
    }
#endif

#ifdef __x86_64__
    if(KernelInited && count > 1024 * 8){
        if(cpu->SupportXSAVE){
            asm volatile("xsave %0" : : "m"(*KernelXsaveSpace), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
            asm volatile("xrstor %0" : : "m"(*fx_area), "a"(UINT32_MAX), "d"(UINT32_MAX) : "memory");
        }else{
            asm volatile("fxsave (%0)" : : "r"(KernelXsaveSpace) : "memory");
            asm volatile("fxrstor (%0)" : : "r"(fx_area) : "memory");
        }
    }
#endif
    return memcmp_fscpuf(buffer1,buffer2,count);
 
}

#ifdef __x86_64__
#pragma GCC pop_options
#endif