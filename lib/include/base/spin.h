#include <atomic/atomic.h>

typedef int32_t spinlock_t;

static inline void spinlock_lock(spinlock_t* l) {
    while(atomic_test_and_set(l,1))
#ifdef __x86_64__
        asm volatile("pause");
#elif defined(__aarch64__)
        asm volatile("yield");
#elif defined (__riscv)
        asm volatile("pause");
#endif
}

static inline void spinlock_unlock(spinlock_t* l) {
    atomic_store_4(l,0,0);
}