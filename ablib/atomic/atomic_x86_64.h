#ifndef ATOMIC_X86_64_H
#define ATOMIC_X86_64_H

/* ---------------- 编译器屏障 ---------------- */
#define _ATOMIC_COMPILER_BARRIER() __asm__ __volatile__("" ::: "memory")

/* ---------------- fence ---------------- */
static inline void atomic_thread_fence(int mo) {
    switch (mo) {
    case ATOMIC_RELAXED:                      break;
    case ATOMIC_CONSUME:
    case ATOMIC_ACQUIRE:
    case ATOMIC_RELEASE:                      _ATOMIC_COMPILER_BARRIER(); break;
    case ATOMIC_ACQ_REL:
    case ATOMIC_SEQ_CST:                      __asm__ __volatile__("mfence" ::: "memory"); break;
    }
}
static inline void atomic_signal_fence(int mo) { (void)mo; _ATOMIC_COMPILER_BARRIER(); }

/* ---------------- load (普通 MOV，TSO 下已具备 acquire) ---------------- */
static inline uint8_t  atomic_load_1(const volatile void *p, int mo) {
    (void)mo; uint8_t v;
    __asm__ __volatile__("movb %1, %0" : "=q"(v) : "m"(*(const volatile uint8_t*)p) : "memory"); return v;
}
static inline uint16_t atomic_load_2(const volatile void *p, int mo) {
    (void)mo; uint16_t v;
    __asm__ __volatile__("movzwl %1, %0" : "=r"(v) : "m"(*(const volatile uint16_t*)p) : "memory"); return v;
}
static inline uint32_t atomic_load_4(const volatile void *p, int mo) {
    (void)mo; uint32_t v;
    __asm__ __volatile__("movl %1, %0" : "=r"(v) : "m"(*(const volatile uint32_t*)p) : "memory"); return v;
}
static inline uint64_t atomic_load_8(const volatile void *p, int mo) {
    (void)mo; uint64_t v;
    __asm__ __volatile__("movq %1, %0" : "=r"(v) : "m"(*(const volatile uint64_t*)p) : "memory"); return v;
}

/* ---------------- store (SEQ_CST 用 xchg 提供 store-load 顺序) ---------------- */
static inline void atomic_store_1(volatile void *p, uint8_t  v, int mo) {
    if (mo == ATOMIC_SEQ_CST)
        __asm__ __volatile__("xchgb %0, %1" : "+q"(v), "+m"(*(volatile uint8_t*)p) :: "memory");
    else
        __asm__ __volatile__("movb %1, %0" : "=m"(*(volatile uint8_t*)p) : "q"(v) : "memory");
}
static inline void atomic_store_2(volatile void *p, uint16_t v, int mo) {
    if (mo == ATOMIC_SEQ_CST)
        __asm__ __volatile__("xchgw %0, %1" : "+r"(v), "+m"(*(volatile uint16_t*)p) :: "memory");
    else
        __asm__ __volatile__("movw %1, %0" : "=m"(*(volatile uint16_t*)p) : "r"(v) : "memory");
}
static inline void atomic_store_4(volatile void *p, uint32_t v, int mo) {
    if (mo == ATOMIC_SEQ_CST)
        __asm__ __volatile__("xchgl %0, %1" : "+r"(v), "+m"(*(volatile uint32_t*)p) :: "memory");
    else
        __asm__ __volatile__("movl %1, %0" : "=m"(*(volatile uint32_t*)p) : "r"(v) : "memory");
}
static inline void atomic_store_8(volatile void *p, uint64_t v, int mo) {
    if (mo == ATOMIC_SEQ_CST)
        __asm__ __volatile__("xchgq %0, %1" : "+r"(v), "+m"(*(volatile uint64_t*)p) :: "memory");
    else
        __asm__ __volatile__("movq %1, %0" : "=m"(*(volatile uint64_t*)p) : "r"(v) : "memory");
}

/* ---------------- exchange (xchg 自带 SEQ_CST) ---------------- */
static inline uint8_t  atomic_exchange_1(volatile void *p, uint8_t  v, int mo) {
    (void)mo; __asm__ __volatile__("xchgb %0, %1" : "+q"(v), "+m"(*(volatile uint8_t*)p) :: "memory"); return v;
}
static inline uint16_t atomic_exchange_2(volatile void *p, uint16_t v, int mo) {
    (void)mo; __asm__ __volatile__("xchgw %0, %1" : "+r"(v), "+m"(*(volatile uint16_t*)p) :: "memory"); return v;
}
static inline uint32_t atomic_exchange_4(volatile void *p, uint32_t v, int mo) {
    (void)mo; __asm__ __volatile__("xchgl %0, %1" : "+r"(v), "+m"(*(volatile uint32_t*)p) :: "memory"); return v;
}
static inline uint64_t atomic_exchange_8(volatile void *p, uint64_t v, int mo) {
    (void)mo; __asm__ __volatile__("xchgq %0, %1" : "+r"(v), "+m"(*(volatile uint64_t*)p) :: "memory"); return v;
}

/* ---------------- compare_exchange (lock cmpxchg) ---------------- */
static inline bool atomic_compare_exchange_1(volatile void *p, void *e, uint8_t d,
                                              bool weak, int sm, int fm) {
    (void)weak; (void)sm; (void)fm;
    uint8_t exp = *(uint8_t*)e;
    bool ok;
    __asm__ __volatile__("lock cmpxchgb %3, %1\n\tsete %b2"
                         : "+a"(exp), "+m"(*(volatile uint8_t*)p), "=q"(ok)
                         : "q"(d) : "memory", "cc");
    if (!ok) *(uint8_t*)e = exp;
    return ok;
}
static inline bool atomic_compare_exchange_2(volatile void *p, void *e, uint16_t d,
                                              bool weak, int sm, int fm) {
    (void)weak; (void)sm; (void)fm;
    uint16_t exp = *(uint16_t*)e;
    bool ok;
    __asm__ __volatile__("lock cmpxchgw %3, %1\n\tsete %b2"
                         : "+a"(exp), "+m"(*(volatile uint16_t*)p), "=q"(ok)
                         : "r"(d) : "memory", "cc");
    if (!ok) *(uint16_t*)e = exp;
    return ok;
}
static inline bool atomic_compare_exchange_4(volatile void *p, void *e, uint32_t d,
                                              bool weak, int sm, int fm) {
    (void)weak; (void)sm; (void)fm;
    uint32_t exp = *(uint32_t*)e;
    bool ok;
    __asm__ __volatile__("lock cmpxchgl %3, %1\n\tsete %b2"
                         : "+a"(exp), "+m"(*(volatile uint32_t*)p), "=q"(ok)
                         : "r"(d) : "memory", "cc");
    if (!ok) *(uint32_t*)e = exp;
    return ok;
}
static inline bool atomic_compare_exchange_8(volatile void *p, void *e, uint64_t d,
                                              bool weak, int sm, int fm) {
    (void)weak; (void)sm; (void)fm;
    uint64_t exp = *(uint64_t*)e;
    bool ok;
    __asm__ __volatile__("lock cmpxchgq %3, %1\n\tsete %b2"
                         : "+a"(exp), "+m"(*(volatile uint64_t*)p), "=q"(ok)
                         : "r"(d) : "memory", "cc");
    if (!ok) *(uint64_t*)e = exp;
    return ok;
}

/* ---------------- fetch_add (lock xadd) ---------------- */
static inline uint8_t  atomic_fetch_add_1(volatile void *p, uint8_t  v, int mo) {
    (void)mo; __asm__ __volatile__("lock xaddb %0, %1" : "+q"(v), "+m"(*(volatile uint8_t*)p) :: "memory"); return v;
}
static inline uint16_t atomic_fetch_add_2(volatile void *p, uint16_t v, int mo) {
    (void)mo; __asm__ __volatile__("lock xaddw %0, %1" : "+r"(v), "+m"(*(volatile uint16_t*)p) :: "memory"); return v;
}
static inline uint32_t atomic_fetch_add_4(volatile void *p, uint32_t v, int mo) {
    (void)mo; __asm__ __volatile__("lock xaddl %0, %1" : "+r"(v), "+m"(*(volatile uint32_t*)p) :: "memory"); return v;
}
static inline uint64_t atomic_fetch_add_8(volatile void *p, uint64_t v, int mo) {
    (void)mo; __asm__ __volatile__("lock xaddq %0, %1" : "+r"(v), "+m"(*(volatile uint64_t*)p) :: "memory"); return v;
}

static inline uint8_t  atomic_add_fetch_1(volatile void *p, uint8_t  v, int mo) { return atomic_fetch_add_1(p, v, mo) + v; }
static inline uint16_t atomic_add_fetch_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_add_2(p, v, mo) + v; }
static inline uint32_t atomic_add_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_add_4(p, v, mo) + v; }
static inline uint64_t atomic_add_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_add_8(p, v, mo) + v; }

/* ---------------- fetch_sub (xadd -v) ---------------- */
static inline uint8_t  atomic_fetch_sub_1(volatile void *p, uint8_t  v, int mo) { return atomic_fetch_add_1(p, (uint8_t)(-v), mo); }
static inline uint16_t atomic_fetch_sub_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_add_2(p, (uint16_t)(-v), mo); }
static inline uint32_t atomic_fetch_sub_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_add_4(p, (uint32_t)(-v), mo); }
static inline uint64_t atomic_fetch_sub_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_add_8(p, (uint64_t)(-v), mo); }

static inline uint8_t  atomic_sub_fetch_1(volatile void *p, uint8_t  v, int mo) { return atomic_fetch_sub_1(p, v, mo) - v; }
static inline uint16_t atomic_sub_fetch_2(volatile void *p, uint16_t v, int mo) { return atomic_fetch_sub_2(p, v, mo) - v; }
static inline uint32_t atomic_sub_fetch_4(volatile void *p, uint32_t v, int mo) { return atomic_fetch_sub_4(p, v, mo) - v; }
static inline uint64_t atomic_sub_fetch_8(volatile void *p, uint64_t v, int mo) { return atomic_fetch_sub_8(p, v, mo) - v; }

/* ---------------- fetch_and/or/xor/nand (CAS loop) ---------------- */
#define _ATOMIC_FETCH_BITOP_X86(SUFFIX, TYPE, OP, EXPR) \
static inline TYPE atomic_fetch_##OP##_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    TYPE old = *(volatile TYPE*)p, neu; \
    do { neu = (EXPR); } \
    while (!atomic_compare_exchange_##SUFFIX(p, &old, neu, false, mo, mo)); \
    return old; \
} \
static inline TYPE atomic_##OP##_fetch_##SUFFIX(volatile void *p, TYPE v, int mo) { \
    TYPE old = *(volatile TYPE*)p, neu; \
    do { neu = (EXPR); } \
    while (!atomic_compare_exchange_##SUFFIX(p, &old, neu, false, mo, mo)); \
    return neu; \
}

_ATOMIC_FETCH_BITOP_X86(1, uint8_t,  and, (uint8_t)(old & v))
_ATOMIC_FETCH_BITOP_X86(2, uint16_t, and, (uint16_t)(old & v))
_ATOMIC_FETCH_BITOP_X86(4, uint32_t, and, (uint32_t)(old & v))
_ATOMIC_FETCH_BITOP_X86(8, uint64_t, and, (uint64_t)(old & v))

_ATOMIC_FETCH_BITOP_X86(1, uint8_t,  or,  (uint8_t)(old | v))
_ATOMIC_FETCH_BITOP_X86(2, uint16_t, or,  (uint16_t)(old | v))
_ATOMIC_FETCH_BITOP_X86(4, uint32_t, or,  (uint32_t)(old | v))
_ATOMIC_FETCH_BITOP_X86(8, uint64_t, or,  (uint64_t)(old | v))

_ATOMIC_FETCH_BITOP_X86(1, uint8_t,  xor, (uint8_t)(old ^ v))
_ATOMIC_FETCH_BITOP_X86(2, uint16_t, xor, (uint16_t)(old ^ v))
_ATOMIC_FETCH_BITOP_X86(4, uint32_t, xor, (uint32_t)(old ^ v))
_ATOMIC_FETCH_BITOP_X86(8, uint64_t, xor, (uint64_t)(old ^ v))

_ATOMIC_FETCH_BITOP_X86(1, uint8_t,  nand, (uint8_t)~(old & v))
_ATOMIC_FETCH_BITOP_X86(2, uint16_t, nand, (uint16_t)~(old & v))
_ATOMIC_FETCH_BITOP_X86(4, uint32_t, nand, (uint32_t)~(old & v))
_ATOMIC_FETCH_BITOP_X86(8, uint64_t, nand, (uint64_t)~(old & v))

#undef _ATOMIC_FETCH_BITOP_X86

/* ---------------- test_and_set / clear ---------------- */
/* flag 设为 1，返回旧值（0 或 1） */
static inline bool atomic_test_and_set(volatile void *p, int mo) {
    (void)mo;
    uint8_t v = 1;
    __asm__ __volatile__("xchgb %0, %1" : "+q"(v), "+m"(*(volatile uint8_t*)p) :: "memory");
    return v != 0;
}
static inline void atomic_clear(volatile void *p, int mo) {
    atomic_store_1(p, 0, mo);
}

/* lock-free 查询 */
static inline bool atomic_is_lock_free(size_t n)    { return n <= 8; }
static inline bool atomic_always_lock_free(size_t n){ return n <= 8; }

#endif /* ATOMIC_X86_64_H */