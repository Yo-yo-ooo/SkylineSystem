#include <klib/klib.h>
#include <klib/kio.h>
#include <conf.h>
#ifdef __x86_64__
#include <arch/x86_64/smp/smp.h>
#include "../../../x86mem/x86mem.h"
#endif

#pragma GCC target("sse,avx")

void _memcpy_128(void* src, void* dest, int64_t size)
{
	auto _src = (__uint128_t*)src;
	auto _dest = (__uint128_t*)dest;
	size >>= 4; // size /= 16
	while (size--)
		*(_dest++) = *(_src++);
}

void _memcpy(void* src, void* dest, uint64_t size)
{
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMCPY)
    if(smp_started != false && this_cpu()->SupportSIMD){
        AVX_memcpy(dest,src,size);
        return;
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
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMSET)
    if(smp_started != false && this_cpu()->SupportSIMD){

        AVX_memset(dest,value,size);
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
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMMOVE)
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
#if defined(__x86_64__) && defined(CONFIG_FAST_MEMCMP)
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

#pragma GCC pop_options