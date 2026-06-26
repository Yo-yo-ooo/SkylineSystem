/*
* SPDX-License-Identifier: GPL-2.0-only
* File: meme.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*/

#include <klib/klib.h>

extern "C" {

// 解决 Strict Aliasing 问题，并允许非对齐访问（编译器会自动拆解为对齐指令如果在严格对齐的架构上）
typedef uint64_t __attribute__((__may_alias__, aligned(1))) unaligned_uint64_t;
typedef uint32_t __attribute__((__may_alias__, aligned(1))) unaligned_uint32_t;
typedef uint16_t __attribute__((__may_alias__, aligned(1))) unaligned_uint16_t;

func_optimize(3) void *memset_fscpuf(void *dst, const int32_t val, size_t n) {
    void *ret = dst;
    uint8_t *d = (uint8_t *)dst;
    uint8_t c = (uint8_t)val;

#if defined(__x86_64__)
    if (n >= 48) {
        asm volatile ("cld\n"
                      "rep stosb"
                      : "+D"(dst), "+c"(n)
                      : "a"(val)
                      : "memory");
        return ret;
    }
#else
    if (n >= sizeof(uint64_t)) {
        // 头部对齐：将 d 推进到 8 字节对齐的边界
        size_t align = (uintptr_t)d & 7;
        if (align) {
            size_t head = 8 - align;
            if (head > n) head = n;
            for (size_t i = 0; i < head; i++) d[i] = c;
            d += head;
            n -= head;
        }

        // 生成 64 位填充掩码
        uint64_t val64 = (uint64_t)c;
        val64 |= val64 << 8;
        val64 |= val64 << 16;
        val64 |= val64 << 32;

        // 主循环展开：每次写 64 字节
        while (n >= 64) {
            ((unaligned_uint64_t *)d)[0] = val64;
            ((unaligned_uint64_t *)d)[1] = val64;
            ((unaligned_uint64_t *)d)[2] = val64;
            ((unaligned_uint64_t *)d)[3] = val64;
            ((unaligned_uint64_t *)d)[4] = val64;
            ((unaligned_uint64_t *)d)[5] = val64;
            ((unaligned_uint64_t *)d)[6] = val64;
            ((unaligned_uint64_t *)d)[7] = val64;
            d += 64;
            n -= 64;
        }

        // 处理剩余的 8 字节块
        while (n >= 8) {
            *(unaligned_uint64_t *)d = val64;
            d += 8;
            n -= 8;
        }
    }
#endif

    // 尾部字节
    while (n--) *d++ = c;
    return ret;
}

func_optimize(3) void *memcpy_fscpuf(void *dst, const void *src, size_t n) {
    void *ret = dst;
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

#if defined(__x86_64__)
    if (n >= 16) {
        asm volatile ("rep movsb"
                      : "+D"(dst), "+S"(src), "+c"(n)
                      :: "memory");
        return ret;
    }
#else
    if (n >= sizeof(uint64_t)) {
        // 头部对齐
        size_t align = (uintptr_t)d & 7;
        if (align) {
            size_t head = 8 - align;
            if (head > n) head = n;
            for (size_t i = 0; i < head; i++) d[i] = s[i];
            d += head;
            s += head;
            n -= head;
        }

        // 主循环展开：每次拷贝 64 字节
        while (n >= 64) {
            ((unaligned_uint64_t *)d)[0] = ((const unaligned_uint64_t *)s)[0];
            ((unaligned_uint64_t *)d)[1] = ((const unaligned_uint64_t *)s)[1];
            ((unaligned_uint64_t *)d)[2] = ((const unaligned_uint64_t *)s)[2];
            ((unaligned_uint64_t *)d)[3] = ((const unaligned_uint64_t *)s)[3];
            ((unaligned_uint64_t *)d)[4] = ((const unaligned_uint64_t *)s)[4];
            ((unaligned_uint64_t *)d)[5] = ((const unaligned_uint64_t *)s)[5];
            ((unaligned_uint64_t *)d)[6] = ((const unaligned_uint64_t *)s)[6];
            ((unaligned_uint64_t *)d)[7] = ((const unaligned_uint64_t *)s)[7];
            d += 64;
            s += 64;
            n -= 64;
        }

        // 8 字贝块
        while (n >= 8) {
            *(unaligned_uint64_t *)d = *(const unaligned_uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }
#endif

    // 尾部字节
    while (n--) *d++ = *s++;
    return ret;
}

func_optimize(3) void *memmove_fscpuf(void *dst, const void *src, size_t n) {
    if (dst == src || n == 0) return dst;

    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (s > d) {
        // 正向拷贝逻辑与 memcpy 一致
#if defined(__x86_64__)
        if (n >= 16) {
            asm volatile ("cld\n"
                          "rep movsb\n"
                          : "+D"(dst), "+S"(src), "+c"(n)
                          :: "memory");
            return dst;
        }
#else
        return memcpy_fscpuf(dst, src, n);
#endif
    } else {
        // 反向拷贝
#if defined(__x86_64__)
        if (n >= 16) {
            void *dst_back = &((uint8_t *)dst)[n - 1];
            const void *src_back = &((const uint8_t *)src)[n - 1];

            asm volatile ("std\n"
                          "rep movsb\n"
                          "cld"
                          : "+D"(dst_back), "+S"(src_back), "+c"(n)
                          :: "memory");
            return dst;
        }
#else
        d += n;
        s += n;

        // 尾部对齐：将 d 逆向推进到 8 字节对齐边界
        size_t align = (uintptr_t)d & 7;
        if (align) {
            size_t tail = align;
            if (tail > n) tail = n;
            for (size_t i = 0; i < tail; i++) *--d = *--s;
            n -= tail;
        }

        // 反向主循环展开
        while (n >= 64) {
            d -= 64; s -= 64;
            ((unaligned_uint64_t *)d)[0] = ((const unaligned_uint64_t *)s)[0];
            ((unaligned_uint64_t *)d)[1] = ((const unaligned_uint64_t *)s)[1];
            ((unaligned_uint64_t *)d)[2] = ((const unaligned_uint64_t *)s)[2];
            ((unaligned_uint64_t *)d)[3] = ((const unaligned_uint64_t *)s)[3];
            ((unaligned_uint64_t *)d)[4] = ((const unaligned_uint64_t *)s)[4];
            ((unaligned_uint64_t *)d)[5] = ((const unaligned_uint64_t *)s)[5];
            ((unaligned_uint64_t *)d)[6] = ((const unaligned_uint64_t *)s)[6];
            ((unaligned_uint64_t *)d)[7] = ((const unaligned_uint64_t *)s)[7];
        }

        while (n >= 8) {
            d -= 8; s -= 8;
            *(unaligned_uint64_t *)d = *(const unaligned_uint64_t *)s;
        }

        // 处理剩余字节
        while (n--) *--d = *--s;
#endif
    }

    return dst;
}

// 完全修复了原有的 TODO: Fix 的问题。
// 原代码直接相减是错误且不符合 C 标准的，本实现使用异或与 CTZ 快速定位不同字节
func_optimize(3)
int32_t memcmp_fscpuf(const void *left, const void *right, size_t len) {
    const uint8_t *l = (const uint8_t *)left;
    const uint8_t *r = (const uint8_t *)right;

    // 8 字节块比较
    while (len >= 8) {
        uint64_t l64 = *(const unaligned_uint64_t *)l;
        uint64_t r64 = *(const unaligned_uint64_t *)r;
        if (l64 != r64) {
            // 异或找差异，__builtin_ctzll 找最低位 1 的位置，除以 8 得到字节偏移
            uint64_t diff = l64 ^ r64;
            int byte_pos = __builtin_ctzll(diff) / 8;
            return (int32_t)l[byte_pos] - (int32_t)r[byte_pos];
        }
        l += 8;
        r += 8;
        len -= 8;
    }

    // 4 字节块比较
    while (len >= 4) {
        uint32_t l32 = *(const unaligned_uint32_t *)l;
        uint32_t r32 = *(const unaligned_uint32_t *)r;
        if (l32 != r32) {
            uint32_t diff = l32 ^ r32;
            int byte_pos = __builtin_ctz(diff) / 8;
            return (int32_t)l[byte_pos] - (int32_t)r[byte_pos];
        }
        l += 4;
        r += 4;
        len -= 4;
    }

    // 逐字节比较
    while (len--) {
        if (*l != *r) {
            return (int32_t)*l - (int32_t)*r;
        }
        l++;
        r++;
    }

    return 0;
}


func_optimize(3) void bzero(void *dst, size_t n) {
    uint8_t *d = (uint8_t *)dst;

#if defined(__x86_64__)
    if (n >= 48) {
        asm volatile ("cld\n"
                      "rep stosb"
                      : "+D"(dst), "+c"(n)
                      : "a"(0)
                      : "memory");
        return;
    }
#elif defined(__riscv64)
    uint16_t cbo_size = 0;
    with_preempt_disabled({
        cbo_size = this_cpu()->cbo_size;
    });

    if (__builtin_expect(cbo_size != 0, 1)) {
        if (has_align((uint64_t)dst, cbo_size)) {
            while (n >= cbo_size) {
                asm volatile ("cbo.zero (%0)" :: "r"(dst));
                dst += cbo_size;
                n -= cbo_size;
            }
        }
    }
#endif

    // 通用清零逻辑（对齐 + 展开，编译器会自动产生 AArch64 的 stp xzr, xzr）
    if (n >= sizeof(uint64_t)) {
        size_t align = (uintptr_t)d & 7;
        if (align) {
            size_t head = 8 - align;
            if (head > n) head = n;
            for (size_t i = 0; i < head; i++) d[i] = 0;
            d += head;
            n -= head;
        }

        while (n >= 64) {
            ((unaligned_uint64_t *)d)[0] = 0;
            ((unaligned_uint64_t *)d)[1] = 0;
            ((unaligned_uint64_t *)d)[2] = 0;
            ((unaligned_uint64_t *)d)[3] = 0;
            ((unaligned_uint64_t *)d)[4] = 0;
            ((unaligned_uint64_t *)d)[5] = 0;
            ((unaligned_uint64_t *)d)[6] = 0;
            ((unaligned_uint64_t *)d)[7] = 0;
            d += 64;
            n -= 64;
        }

        while (n >= 8) {
            *(unaligned_uint64_t *)d = 0;
            d += 8;
            n -= 8;
        }
    }

    while (n--) *d++ = 0;
}

} // extern "C"