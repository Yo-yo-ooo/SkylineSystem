#include <klib/klib.h>

void _memcpy_128(void* src, void* dest, int64_t size)
{
	// __uint128_t is 16 bytes
	auto _src = (__uint128_t*)src;
	auto _dest = (__uint128_t*)dest;
	size >>= 4; // size /= 16
	while (size--)
		*(_dest++) = *(_src++);
}

void _memcpy(void* src, void* dest, uint64_t size)
{
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
    /* char* _dest = (char*)dest;
    while (size--)
        *(_dest++) = *(_src++); */
    
    uint8_t* p = static_cast<uint8_t*>(dest);
    //uint8_t x = value & 0xff;
    //size_t sz = static_cast<size_t>(n);
    size_t leftover = size & 0x7;

    /* Catch the pathological case of 0. */
    if (!size)
        return;

    /* To understand what's going on here, take a look at the original
     * bytewise_memset and consider unrolling the loop. For this situation
     * we'll unroll the loop 8 times (assuming a 32-bit architecture). Choosing
     * the level to which to unroll the loop can be a fine art...
     */
    size = (size + 7) >> 3;
    switch (leftover) {
        case 0: do { *p++ = *_src++;
        case 7:      *p++ = *_src++;
        case 6:      *p++ = *_src++;
        case 5:      *p++ = *_src++;
        case 4:      *p++ = *_src++;
        case 3:      *p++ = *_src++;
        case 2:      *p++ = *_src++;
        case 1:      *p++ = *_src++;
                } while (--size > 0);
    }
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
	if (size & 0xFFE0)//(size >= 32)
	{
		uint64_t s2 = size & 0xFFFFFFF0;
		_memset_128(dest, value, s2);
		if (!s2)
			return;
		dest = (void*)((uint64_t)dest + s2);
		size = size & 0xF;
	}

    /* char* d = (char*)dest;
    for (uint64_t i = 0; i < size; i++)
        *(d++) = value; */
    uint8_t* p = static_cast<uint8_t*>(dest);
    uint8_t x = value & 0xff;
    //size_t sz = static_cast<size_t>(n);
    size_t leftover = size & 0x7;

    /* Catch the pathological case of 0. */
    if (!size)
        return;

    /* To understand what's going on here, take a look at the original
     * bytewise_memset and consider unrolling the loop. For this situation
     * we'll unroll the loop 8 times (assuming a 32-bit architecture). Choosing
     * the level to which to unroll the loop can be a fine art...
     */
    size = (size + 7) >> 3;
    switch (leftover) {
        case 0: do { *p++ = x;
        case 7:      *p++ = x;
        case 6:      *p++ = x;
        case 5:      *p++ = x;
        case 4:      *p++ = x;
        case 3:      *p++ = x;
        case 2:      *p++ = x;
        case 1:      *p++ = x;
                } while (--size > 0);
    }
    //return dest;
}


void _memmove(void* src, void* dest, uint64_t size) {
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
    const u8 *p1 = (const u8 *)buffer1;
    const u8 *p2 = (const u8 *)buffer2;
    for (size_t i = 0; i < count; i++) {
        if (p1[i] != p2[i]) {
        return p1[i] < p2[i] ? -1 : 1;
        }
    }

    return 0;
 
}

