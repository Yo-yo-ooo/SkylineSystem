#pragma once
#ifndef _KLIB_RANDOM_H_
#define _KLIB_RANDOM_H_

#include <stdint.h>
#include <stddef.h>

namespace RND
{
    extern __uint128_t g_lehmer64_state;

    uint64_t lehmer64();

    double RandomDouble();

    uint64_t RandomInt();

    static inline uint32_t lcg_random(int32_t seed)
    {
        const long a = 1103515245;
        const long c = 12345;
        const long m = (long) (((unsigned long) 1 << 31) - 1);
        return ((a * seed + c) % m) + 1;
    }

    static inline int32_t rand(int32_t seed, int32_t min, int32_t max)
    {
        return min + lcg_random(seed) % (max - min + 1);
    }

};

#endif