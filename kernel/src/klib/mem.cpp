#include <klib/klib.h>
#include <klib/kio.h>
#include <conf.h>
#if defined(__x86_64__) && NOT_COMPILE_X86MEM == 0
#include "../../../x86mem/x86mem.h"
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
#include <emmintrin.h>
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
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMCPY) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && this_cpu()->SupportSIMD){
        AVX_memcpy(dest,src,size);
        return;
    }
#elif defined(__x86_64__)

    if(smp_started != false && this_cpu()->SupportSIMD){
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
#endif
	if (size & 0xFFE0)//(size >= 32)
	{
		uint64_t s2 = size & 0xFFFFFFF0;
		_memcpy_128(src, dest, s2);
		if (!s2)
			return;
		src = (void*)((uint64_t)src + s2);
		dest = (void*)((uint64_t)dest + s2);
		size = size & 0xF;
	}

    const char* _src  = (const char*)src;
    char* _dest = (char*)dest;
    while (size--)
        *(_dest++) = *(_src++);
}

void _memset_128(void* dest, uint8_t value, int64_t size)
{
	// __uint128_t is 16 bytes
	auto _dest = (__uint128_t*)dest;

	// Create a 128 bit value with the byte value in each byte
	__uint128_t val = value;
	val |= val << 8;
	val |= val << 16;
	val |= val << 32;
	val |= val << 64;

	size >>= 4; // size /= 16
	while (size--)
		*(_dest++) = val;
}

void _memset(void* dest, uint8_t value, uint64_t size)
{
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMSET) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && this_cpu()->SupportSIMD){
        AVX_memset(dest,value,size);
        return;
    }
#elif defined(__x86_64__)
    if(smp_started != false && this_cpu()->SupportSIMD){
        uint64_t Loop128C = size / 128;
        __m128i val = _mm_set1_epi8((char)value);
        for(uint64_t i = 0; i < Loop128C; i++){
            _mm_store_si128((__m128i*)((uint64_t)dest + i * 16), val);
            size -= 16;
        }
        if(size % 128){
            char* d = (char*)dest;
            for (uint64_t i = 0; i < size; i++)
                *(d++) = value;
        }
        return;
    }
#endif

	if (size & 0xFFE0)//(size >= 32)
	{
		uint64_t s2 = size & 0xFFFFFFF0;
		_memset_128(dest, value, s2);
		if (!s2)
			return;
		dest = (void*)((uint64_t)dest + s2);
		size = size & 0xF;
	}

    char* d = (char*)dest;
    for (uint64_t i = 0; i < size; i++)
        *(d++) = value;
}


void _memmove(void* src, void* dest, uint64_t size) {
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMMOVE) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && this_cpu()->SupportSIMD){
        AVX_memmove(dest,src,size);
        return;
    }
#endif
	char* d = (char*) dest;
	char* s = (char*) src;
	if(d < s) {
		while(size--) {
			*d++ = *s++;
		}
	} else {
		d += size;
		s += size;
		while(size--) {
			*--d = *--s;
		}
	}
}

int32_t _memcmp(const void* buffer1,const void* buffer2,size_t  count)
{
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMCMP) && NOT_COMPILE_X86MEM == 0
    if(smp_started != false && this_cpu()->SupportSIMD){
        return AVX_memcmp(buffer1,buffer2,count,1);
    }
#endif
    const u8 *p1 = (const u8 *)buffer1;
    const u8 *p2 = (const u8 *)buffer2;
    for (size_t i = 0; i < count; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
 
}

#ifdef __x86_64__
#pragma GCC pop_options
#endif