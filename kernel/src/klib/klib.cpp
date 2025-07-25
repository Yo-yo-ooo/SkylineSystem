#include <klib/klib.h>
#include <stdint.h>
#include <stddef.h>

void Panic(const char* message){
    e9_print("Panic!");
    e9_printf(message);
    hcf();
}

void Panic(bool halt, const char* message){
    e9_print("Panic!");
    e9_printf(message);
    if(halt){
        hcf();
    }
}

void Panic(const char* message,bool halt){
    e9_print("Panic!");
    e9_printf(message);
    if(halt){
        hcf();
    }
}

// Halt and catch fire function.
void hcf(void) {
    for (;;) {
#ifdef __x86_64__
        asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm ("wfi");
#elif defined (__loongarch64)
        asm ("idle 0");
#endif
    }
}



void spinlock_lock(spinlock_t* l) {
    while (__sync_lock_test_and_set(l, 1)){
#if defined(__x86_64__)
        __asm__ volatile("pause");
#else
        ;
#endif
    }
}

void spinlock_unlock(spinlock_t* l) {
    __sync_lock_release(l);
}

void *__memcpy(void *__restrict__ d, const void *__restrict__ s, size_t n) { 
    //_memcpy(s, d, (uint64_t)n);
    return _EXT4_T_memcpy(d,s,n);
}

void bitmap_set(u8* bitmap, u64 bit) {
#if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    bitmap[bit / 8] |= 1 << (bit % 8);
#elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    bitmap[bit / 8] = __builtin_bswap64(bitmap[bit/8]);
    bitmap[bit / 8] |= 1 << (bit % 8);
    bitmap[bit / 8] = __builtin_bswap64(bitmap[bit/8]);
#else
#error "Unknown endianness"
#endif
}

void bitmap_clear(u8* bitmap, u64 bit) {
#if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    bitmap[bit / 8] &= ~(1 << (bit % 8));
#elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    bitmap[bit / 8] = __builtin_bswap64(bitmap[bit / 8] & (~(1 << (bit % 8))));
#else
#error "Unknown endianness"
#endif
}

bool bitmap_get(u8* bitmap, u64 bit) {
#if __BYTE_ORDER__==__ORDER_LITTLE_ENDIAN__
    return (bitmap[bit / 8] & (1 << (bit % 8))) != 0;
#elif __BYTE_ORDER__==__ORDER_BIG_ENDIAN__
    return (bitmap[bit / 8] & (1 >> (bit % 8))) != 0;
#else
#error "Unknown endianness"
#endif
}



void *_EXT4_T_memcpy(void *__restrict__ dest, const void *__restrict__ src, size_t n)
{
	uint8_t *d = dest;
	const uint8_t *s = src;

#ifdef __GNUC__

#if __BYTE_ORDER == __LITTLE_ENDIAN
#define LS >>
#define RS <<
#else
#define LS <<
#define RS >>
#endif

	typedef uint32_t __attribute__((__may_alias__)) u32;
	uint32_t w, x;

	for (; (uintptr_t)s % 4 && n; n--) *d++ = *s++;

	if ((uintptr_t)d % 4 == 0) {
		for (; n>=16; s+=16, d+=16, n-=16) {
			*(u32 *)(d+0) = *(u32 *)(s+0);
			*(u32 *)(d+4) = *(u32 *)(s+4);
			*(u32 *)(d+8) = *(u32 *)(s+8);
			*(u32 *)(d+12) = *(u32 *)(s+12);
		}
		if (n&8) {
			*(u32 *)(d+0) = *(u32 *)(s+0);
			*(u32 *)(d+4) = *(u32 *)(s+4);
			d += 8; s += 8;
		}
		if (n&4) {
			*(u32 *)(d+0) = *(u32 *)(s+0);
			d += 4; s += 4;
		}
		if (n&2) {
			*d++ = *s++; *d++ = *s++;
		}
		if (n&1) {
			*d = *s;
		}
		return dest;
	}

	if (n >= 32) switch ((uintptr_t)d % 4) {
	case 1:
		w = *(u32 *)s;
		*d++ = *s++;
		*d++ = *s++;
		*d++ = *s++;
		n -= 3;
		for (; n>=17; s+=16, d+=16, n-=16) {
			x = *(u32 *)(s+1);
			*(u32 *)(d+0) = (w LS 24) | (x RS 8);
			w = *(u32 *)(s+5);
			*(u32 *)(d+4) = (x LS 24) | (w RS 8);
			x = *(u32 *)(s+9);
			*(u32 *)(d+8) = (w LS 24) | (x RS 8);
			w = *(u32 *)(s+13);
			*(u32 *)(d+12) = (x LS 24) | (w RS 8);
		}
		break;
	case 2:
		w = *(u32 *)s;
		*d++ = *s++;
		*d++ = *s++;
		n -= 2;
		for (; n>=18; s+=16, d+=16, n-=16) {
			x = *(u32 *)(s+2);
			*(u32 *)(d+0) = (w LS 16) | (x RS 16);
			w = *(u32 *)(s+6);
			*(u32 *)(d+4) = (x LS 16) | (w RS 16);
			x = *(u32 *)(s+10);
			*(u32 *)(d+8) = (w LS 16) | (x RS 16);
			w = *(u32 *)(s+14);
			*(u32 *)(d+12) = (x LS 16) | (w RS 16);
		}
		break;
	case 3:
		w = *(u32 *)s;
		*d++ = *s++;
		n -= 1;
		for (; n>=19; s+=16, d+=16, n-=16) {
			x = *(u32 *)(s+3);
			*(u32 *)(d+0) = (w LS 8) | (x RS 24);
			w = *(u32 *)(s+7);
			*(u32 *)(d+4) = (x LS 8) | (w RS 24);
			x = *(u32 *)(s+11);
			*(u32 *)(d+8) = (w LS 8) | (x RS 24);
			w = *(u32 *)(s+15);
			*(u32 *)(d+12) = (x LS 8) | (w RS 24);
		}
		break;
	}
	if (n&16) {
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
	}
	if (n&8) {
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
	}
	if (n&4) {
		*d++ = *s++; *d++ = *s++; *d++ = *s++; *d++ = *s++;
	}
	if (n&2) {
		*d++ = *s++; *d++ = *s++;
	}
	if (n&1) {
		*d = *s;
	}
	return dest;
#endif

	for (; n; n--) *d++ = *s++;
	return dest;
}

uint16_t kld_16 (const uint8_t* ptr)	/*	 Load a 2-byte little-endian word */
{
	uint16_t rv;

	rv = ptr[1];
	rv = rv << 8 | ptr[0];
	return rv;
}

uint32_t kld_32 (const uint8_t* ptr)	/* Load a 4-byte little-endian word */
{
	uint32_t rv;

	rv = ptr[3];
	rv = rv << 8 | ptr[2];
	rv = rv << 8 | ptr[1];
	rv = rv << 8 | ptr[0];
	return rv;
}

uint64_t kld_64 (const uint8_t* ptr)	/* Load an 8-byte little-endian word */
{
	uint64_t rv;

	rv = ptr[7];
	rv = rv << 8 | ptr[6];
	rv = rv << 8 | ptr[5];
	rv = rv << 8 | ptr[4];
	rv = rv << 8 | ptr[3];
	rv = rv << 8 | ptr[2];
	rv = rv << 8 | ptr[1];
	rv = rv << 8 | ptr[0];
	return rv;
}