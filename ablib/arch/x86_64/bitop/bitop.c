// bitop.c
#include <stdint.h>

// =============================================================================
// CTZ (Count Trailing Zeros)
// =============================================================================
static int32_t __ctzsi2(uint32_t x) {
    if (x == 0) return 32;

#if defined(__BMI__)
    uint32_t r;
    __asm__ ("tzcnt %1, %0" : "=r"(r) : "r"(x));
    return (int32_t)r;
#else
    uint32_t r;
    __asm__ ("bsf %1, %0" : "=r"(r) : "r"(x));
    return (int32_t)r;
#endif
}

static int32_t __ctzdi2(uint64_t x) {
    if (x == 0) return 64;

#if defined(__BMI__)
    uint64_t r;                                  // <-- 与 x 同宽
    __asm__ ("tzcnt %1, %0" : "=r"(r) : "r"(x));
    return (int32_t)r;
#else
    uint64_t r;                                  // <-- 与 x 同宽
    __asm__ ("bsf %1, %0" : "=r"(r) : "r"(x));   // 生成 bsfq
    return (int32_t)r;
#endif
}

// =============================================================================
// CLZ (Count Leading Zeros)
// =============================================================================
static int32_t __clzsi2(uint32_t x) {
    if (x == 0) return 32;

#if defined(__ABM__) || defined(__BMI__)
    uint32_t r;
    __asm__ ("lzcnt %1, %0" : "=r"(r) : "r"(x));
    return (int32_t)r;
#else
    uint32_t r;
    __asm__ ("bsr %1, %0" : "=r"(r) : "r"(x));
    return 31 - (int32_t)r;
#endif
}

static int32_t __clzdi2(uint64_t x) {
    if (x == 0) return 64;

#if defined(__ABM__) || defined(__BMI__)
    uint64_t r;                                  // <-- 与 x 同宽
    __asm__ ("lzcnt %1, %0" : "=r"(r) : "r"(x));
    return (int32_t)r;
#else
    uint64_t r;                                  // <-- 与 x 同宽
    __asm__ ("bsr %1, %0" : "=r"(r) : "r"(x));   // 生成 bsrq
    return 63 - (int32_t)r;
#endif
}

// =============================================================================
// FFS (Find First Set)
// =============================================================================
static int32_t __ffssi2(int32_t x) {
    return x == 0 ? 0 : __ctzsi2((uint32_t)x) + 1;
}

static int32_t __ffsdi2(int64_t x) {
    return x == 0 ? 0 : __ctzdi2((uint64_t)x) + 1;
}

// =============================================================================
// 对外暴露的包装函数
// =============================================================================
#if defined(__ABM__) || defined(__BMI__)
int32_t ctz32V1(uint32_t x) { return __ctzsi2(x); }
int32_t ctz64V1(uint64_t x) { return __ctzdi2(x); }
int32_t clz32V1(uint32_t x) { return __clzsi2(x); }
int32_t clz64V1(uint64_t x) { return __clzdi2(x); }
#else
int32_t ctz32V0(uint32_t x) { return __ctzsi2(x); }
int32_t ctz64V0(uint64_t x) { return __ctzdi2(x); }
int32_t clz32V0(uint32_t x) { return __clzsi2(x); }
int32_t clz64V0(uint64_t x) { return __clzdi2(x); }
#endif