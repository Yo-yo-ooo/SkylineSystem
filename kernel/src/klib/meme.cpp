/*
* SPDX-License-Identifier: GPL-2.0-only
* File: meme.cpp
* Copyright (C) 2026 Yo-yo-ooo
*
* This file is part of SkylineSystem.
*
* SkylineSystem is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#include <klib/klib.h>

extern "C"{

#define DECL_MEM_COPY_BACK_FUNC(type) \
    func_optimize(3) static inline size_t \
    VAR_CONCAT(_memcpy_bw_, type)(void *const dst, \
                                  const void *const src, \
                                  const size_t n) \
    {                                                                          \
        if (n < sizeof(type)) {                                                \
            return n;                                                          \
        }                                                                      \
                                                                               \
        size_t off = n - sizeof(type);                                  \
        do {                                                                   \
            *((type *)(dst + off)) = *((const type *)(src + off));             \
            if (off < sizeof(type)) {                                          \
                return off;                                                    \
            }                                                                  \
                                                                               \
            off -= sizeof(type);                                               \
        } while (true);                                                        \
                                                                               \
        return n;                                                              \
    }

func_optimize(3) static inline size_t
_memcpy_bw_uint64_t(void *const dst,
                    const void *const src,
                    const size_t n)
{
    if (n < sizeof(uint64_t)) {
        return n;
    }

    size_t off = n - sizeof(uint64_t);
#if defined(__aarch64__)
    if (n >= (2 * sizeof(uint64_t))) {
        off -= sizeof(uint64_t);
        do {
            uint64_t left_ch = 0;
            uint64_t right_ch = 0;

            asm volatile ("ldp %0, %1, [%2]"
                            : "+r"(left_ch), "+r"(right_ch)
                            : "r"((const uint64_t *)(src + off))
                            : "memory");

            asm volatile ("stp %0, %1, [%2]"
                            :: "r"(left_ch), "r"(right_ch),
                                "r"((uint64_t *)(dst + off)));

            if (off < sizeof(uint64_t) * 2) {
                break;
            }

            off -= sizeof(uint64_t) * 2;
        } while (true);

        if (off < sizeof(uint64_t)) {
            return off;
        }
    }
#endif /* defined(__aarch64__) */

    do {
        *(uint64_t *)(dst + off) = *(const uint64_t *)(src + off);
        if (off < sizeof(uint64_t)) {
            break;
        }

        off -= sizeof(uint64_t);
    } while (true);

    return off;
}


DECL_MEM_COPY_BACK_FUNC(uint8_t)
DECL_MEM_COPY_BACK_FUNC(uint16_t)
DECL_MEM_COPY_BACK_FUNC(uint32_t)

func_optimize(3) void *memset_fscpuf(void *dst, const int32_t val, size_t n) {
    void *ret = dst;
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
    uint64_t value64 = 0;
    if (n >= sizeof(uint64_t)) {
        value64 =
            (uint64_t)val << 56
          | (uint64_t)val << 48
          | (uint64_t)val << 40
          | (uint64_t)val << 32
          | (uint64_t)val << 24
          | (uint64_t)val << 16
          | (uint64_t)val << 8
          | (uint64_t)val;

#if defined(__aarch64__)
        while (n >= sizeof(uint64_t) * 2) {
            asm volatile ("stp %0, %0, [%1]" :: "r"(value64), "r"(dst));

            dst += sizeof(uint64_t) * 2;
            n -= sizeof(uint64_t) * 2;
        }
#endif /* defined(__aarch64__) */

        while (n >= sizeof(uint64_t)) {
            *(uint64_t *)dst = value64;

            dst += sizeof(uint64_t);
            n -= sizeof(uint64_t);
        }

        while (n >= sizeof(uint32_t)) {
            *(uint32_t *)dst = value64;

            dst += sizeof(uint32_t);
            n -= sizeof(uint32_t);
        }

        while (n >= sizeof(uint16_t)) {
            *(uint16_t *)dst = value64;

            dst += sizeof(uint16_t);
            n -= sizeof(uint16_t);
        }
    }
#endif /* defined(__x86_64__) */

    const auto __limit__606 = (n); 
    for (auto i = (typeof(n))0; i != __limit__606; i++){
        ((uint8_t *)dst)[i] = (uint8_t)val;
    }

    return ret;
}


#define DECL_MEM_COPY_FUNC(type) \
    func_optimize(3) static inline size_t                            \
    VAR_CONCAT(_memcpy_, type)(void *dst,                                      \
                               const void *src,                                \
                               size_t n,                                \
                               void **const dst_out,                           \
                               const void **const src_out)                     \
    {                                                                          \
        if (n >= sizeof(type)) {                                               \
            do {                                                               \
                *(type *)dst = *(const type *)src;                             \
                                                                               \
                dst += sizeof(type);                                           \
                src += sizeof(type);                                           \
                                                                               \
                n -= sizeof(type);                                             \
            } while (n >= sizeof(type));                                       \
                                                                               \
            *dst_out = dst;                                                    \
            *src_out = src;                                                    \
        }                                                                      \
                                                                               \
        return n;                                                              \
    }

func_optimize(3) static inline size_t
_memcpy_uint64_t(void *dst,
                 const void *src,
                 size_t n,
                 void **const dst_out,
                 const void **const src_out)
{
    if (n >= sizeof(uint64_t)) {
    #if defined(__aarch64__)
        if (n >= sizeof(uint64_t) * 2) {
            do {
                uint64_t left_ch = 0;
                uint64_t right_ch = 0;

                asm volatile ("ldp %0, %1, [%2]"
                              : "+r"(left_ch), "+r"(right_ch)
                              : "r"(src)
                              : "memory");

                asm volatile ("stp %0, %1, [%2]"
                              :: "r"(left_ch), "r"(right_ch), "r"(dst));

                dst += sizeof(uint64_t) * 2;
                src += sizeof(uint64_t) * 2;

                n -= sizeof(uint64_t) * 2;
            } while (n >= sizeof(uint64_t) * 2);

            if (n < sizeof(uint64_t)) {
                *dst_out = dst;
                *src_out = src;

                return n;
            }
        }
    #endif /* defined(__aarch64__) */

        do {
            *(uint64_t *)dst = *(const uint64_t *)src;

            dst += sizeof(uint64_t);
            src += sizeof(uint64_t);

            n -= sizeof(uint64_t);
        } while (n >= sizeof(uint64_t));

        *dst_out = dst;
        *src_out = src;
    }

    return n;
}

DECL_MEM_COPY_FUNC(uint8_t)
DECL_MEM_COPY_FUNC(uint16_t)
DECL_MEM_COPY_FUNC(uint32_t)



func_optimize(3) void *memcpy_fscpuf(void *dst, const void *src, size_t n) {
    void *ret = dst;
#if defined(__x86_64__)
    if (n >= 16) {
        asm volatile ("rep movsb"
                      : "+D"(dst), "+S"(src), "+c"(n)
                      :: "memory");
        return ret;
    }
#endif
    if (n >= sizeof(uint64_t)) {
        n = _memcpy_uint64_t(dst, src, n, &dst, &src);
        n = _memcpy_uint32_t(dst, src, n, &dst, &src);
        n = _memcpy_uint16_t(dst, src, n, &dst, &src);

        _memcpy_uint8_t(dst, src, n, &dst, &src);
    } else if (n >= sizeof(uint32_t)) {
        n = _memcpy_uint32_t(dst, src, n, &dst, &src);
        n = _memcpy_uint16_t(dst, src, n, &dst, &src);

        _memcpy_uint8_t(dst, src, n, &dst, &src);
    } else if (n >= sizeof(uint16_t)) {
        n = _memcpy_uint16_t(dst, src, n, &dst, &src);
        if (n == 0) {
            return dst;
        }

        _memcpy_uint8_t(dst, src, n, &dst, &src);
    } else {
        _memcpy_uint8_t(dst, src, n, &dst, &src);
    }

    return ret;
}

#define DECL_MEM_COPY_BACK_FUNC(type) \
    func_optimize(3) static inline size_t \
    VAR_CONCAT(_memcpy_bw_, type)(void *const dst, \
                                  const void *const src, \
                                  const size_t n) \
    {                                                                          \
        if (n < sizeof(type)) {                                                \
            return n;                                                          \
        }                                                                      \
                                                                               \
        size_t off = n - sizeof(type);                                  \
        do {                                                                   \
            *((type *)(dst + off)) = *((const type *)(src + off));             \
            if (off < sizeof(type)) {                                          \
                return off;                                                    \
            }                                                                  \
                                                                               \
            off -= sizeof(type);                                               \
        } while (true);                                                        \
                                                                               \
        return n;                                                              \
    }



func_optimize(3) void *memmove_fscpuf(void *dst, const void *src, size_t n) {
    void *ret = dst;
    if (src > dst) {
    #if defined(__x86_64__)
        if (n >= 16) {
            asm volatile ("cld\n"
                          "rep movsb\n"
                          : "+D"(dst), "+S"(src), "+c"(n)
                          :: "memory");
            return ret;
        }
    #endif /* defined(__x86_64__) */
        const uint64_t diff = ((uint64_t)(src) - (uint64_t)(dst));
        if (diff >= sizeof(uint64_t)) {
            memcpy_fscpuf(dst, src, n);
        } else if (diff >= sizeof(uint32_t)) {
            n = _memcpy_uint32_t(dst, src, n, &dst, &src);
            n = _memcpy_uint16_t(dst, src, n, &dst, &src);

            _memcpy_uint8_t(dst, src, n, &dst, &src);
        } else if (diff >= sizeof(uint16_t)) {
            n = _memcpy_uint16_t(dst, src, n, &dst, &src);
            _memcpy_uint8_t(dst, src, n, &dst, &src);
        } else if (diff >= sizeof(uint8_t)) {
            _memcpy_uint8_t(dst, src, n, &dst, &src);
        }
    } else {
    #if defined(__x86_64__)
        if (n >= 16) {
            void *dst_back = &((uint8_t *)dst)[n - 1];
            const void *src_back = &((const uint8_t *)src)[n - 1];

            asm volatile ("std\n"
                          "rep movsb\n"
                          "cld"
                          : "+D"(dst_back), "+S"(src_back), "+c"(n)
                          :: "memory");

            return ret;
        }
    #endif /* defined(__x86_64__) */
        const uint64_t diff = ((uint64_t)(dst) - (uint64_t)(src));
        if (diff >= sizeof(uint64_t)) {
            n = _memcpy_bw_uint64_t(dst, src, n);
            n = _memcpy_bw_uint32_t(dst, src, n);
            n = _memcpy_bw_uint16_t(dst, src, n);

            _memcpy_bw_uint8_t(dst, src, n);
        } else if (diff >= sizeof(uint32_t)) {
            n = _memcpy_bw_uint32_t(dst, src, n);
            n = _memcpy_bw_uint16_t(dst, src, n);

            _memcpy_bw_uint8_t(dst, src, n);
        } else if (diff >= sizeof(uint16_t)) {
            n = _memcpy_bw_uint16_t(dst, src, n);
            _memcpy_bw_uint8_t(dst, src, n);
        } else if (diff >= sizeof(uint8_t)) {
            _memcpy_bw_uint8_t(dst, src, n);
        }
    }

    return ret;
}

// TODO: Fix
#define DECL_MEM_CMP_FUNC(type)                                                \
    func_optimize(3) static inline int32_t                                      \
    VAR_CONCAT(_memcmp_, type)(const void *left,                               \
                               const void *right,                              \
                               size_t len,                                     \
                               const void **const left_out,                    \
                               const void **const right_out,                   \
                               size_t *const len_out)                          \
    {                                                                          \
        if (len >= sizeof(type)) {                                             \
            do {                                                               \
                const type left_ch = *(const type *)left;                      \
                const type right_ch = *(const type *)right;                    \
                                                                               \
                if (left_ch != right_ch) {                                     \
                    return (int32_t)(left_ch - right_ch);                          \
                }                                                              \
                                                                               \
                left += sizeof(type);                                          \
                right += sizeof(type);                                         \
                len -= sizeof(type);                                           \
            } while (len >= sizeof(type));                                     \
                                                                               \
            *left_out = left;                                                  \
            *right_out = right;                                                \
            *len_out = len;                                                    \
        }                                                                      \
                                                                               \
        return 0;                                                              \
    }

func_optimize(3) static inline int32_t
_memcmp_uint64_t(const void *left,
                 const void *right,
                 size_t len,
                 const void **const left_out,
                 const void **const right_out,
                 size_t *const len_out)
{
    if (len >= sizeof(uint64_t)) {
    #if defined(__aarch64__)
        if (len >= (2 * sizeof(uint64_t))) {
            do {
                uint64_t left_ch = 0;
                uint64_t right_ch = 0;

                uint64_t left_ch_2 = 0;
                uint64_t right_ch_2 = 0;

                asm volatile ("ldp %0, %1, [%2]"
                              : "+r"(left_ch), "+r"(left_ch_2)
                              : "r"(left)
                              : "memory");

                asm volatile ("ldp %0, %1, [%2]"
                              : "+r"(right_ch), "+r"(right_ch_2)
                              : "r"(right)
                              : "memory");

                if (left_ch != right_ch) {
                    return (int32_t)(left_ch - right_ch);
                }

                if (left_ch_2 != right_ch_2) {
                    return (int32_t)(left_ch_2 - right_ch_2);
                }

                len -= sizeof(uint64_t) * 2;
                left += sizeof(uint64_t) * 2;
                right += sizeof(uint64_t) * 2;
            } while (len >= sizeof(uint64_t) * 2);

            if (len < sizeof(uint64_t)) {
                *left_out = left;
                *right_out = right;
                *len_out = len;

                return 0;
            }
        }
    #endif /* defined(__aarch64__) */

        do {
            const uint64_t left_ch = *(const uint64_t *)left;
            const uint64_t right_ch = *(const uint64_t *)right;

            if (left_ch != right_ch) {
                return (int32_t)(left_ch - right_ch);
            }

            len -= sizeof(uint64_t);
            left += sizeof(uint64_t);
            right += sizeof(uint64_t);
        } while (len >= sizeof(uint64_t));

        *left_out = left;
        *right_out = right;
        *len_out = len;
    }

    return 0;
}

DECL_MEM_CMP_FUNC(uint8_t)
DECL_MEM_CMP_FUNC(uint16_t)
DECL_MEM_CMP_FUNC(uint32_t)


func_optimize(3)
int32_t memcmp_fscpuf(const void *left, const void *right, size_t len) {
    int32_t res = _memcmp_uint64_t(left, right, len, &left, &right, &len);
    if (res != 0) {
        return res;
    }

    res = _memcmp_uint32_t(left, right, len, &left, &right, &len);
    if (res != 0) {
        return res;
    }

    res = _memcmp_uint16_t(left, right, len, &left, &right, &len);
    if (res != 0) {
        return res;
    }

    res = _memcmp_uint8_t(left, right, len, &left, &right, &len);
    return res;
}


func_optimize(3) void bzero(void *dst, size_t n) {
#if defined(__x86_64__)
    if (n >= 48) {
        asm volatile ("cld\n"
                      "rep stosb"
                      : "+D"(dst), "+c"(n)
                      : "a"(0)
                      : "memory");
        return;
    }
#elif defined(__aarch64__)
    if (n >= sizeof(uint64_t) * 2) {
        do {
            asm volatile ("stp xzr, xzr, [%0]" :: "r"(dst));

            dst += sizeof(uint64_t) * 2;
            n -= sizeof(uint64_t) * 2;
        } while (n >= sizeof(uint64_t) * 2);

        if (n == 0) {
            return;
        }
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
    while (n >= sizeof(uint64_t)) {
        *(uint64_t *)dst = 0;

        dst += sizeof(uint64_t);
        n -= sizeof(uint64_t);
    }

    if (n == 0) {
        return;
    }

    while (n >= sizeof(uint32_t)) {
        *(uint32_t *)dst = 0;

        dst += sizeof(uint32_t);
        n -= sizeof(uint32_t);
    }

    if (n == 0) {
        return;
    }

    while (n >= sizeof(uint16_t)) {
        *(uint16_t *)dst = 0;

        dst += sizeof(uint16_t);
        n -= sizeof(uint16_t);
    }

    if (n == 0) {
        return;
    }

    while (n >= sizeof(uint8_t)) {
        *(uint8_t *)dst = 0;

        dst += sizeof(uint8_t);
        n -= sizeof(uint8_t);
    }
}

}