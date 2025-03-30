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



void lock(atomic_lock* l) {
    while (__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE)) {
#if defined(__x86_64__)
        __asm__ volatile("pause");
#else
        ;
#endif
    }
}

void unlock(atomic_lock* l) {
    __atomic_clear(&l->locked, __ATOMIC_RELEASE);
}

void *__memcpy(void *d, const void *s, size_t n) { 
    _memcpy(s, d, (uint64_t)n);
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

