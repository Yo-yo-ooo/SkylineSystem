
#include <klib/klib.h>


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