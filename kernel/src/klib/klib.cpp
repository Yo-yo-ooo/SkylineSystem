#include <klib/klib.h>
#include <stdint.h>
#include <stddef.h>

void Panic(const char* message){
    kerrorln("Panic!");
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
        asm volatile("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm volatile("wfi");
#elif defined (__loongarch64)
        asm volatile("idle 0");
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

void *__memcpy(void * d, const void * s, uint64_t n) { 
    _memcpy(s, d, n);
    return d;
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