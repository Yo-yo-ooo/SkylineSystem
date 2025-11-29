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
};

#endif