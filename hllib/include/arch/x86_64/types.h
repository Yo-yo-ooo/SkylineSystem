#pragma once
#ifndef _ARCH_X86_64_TYPE_H_
#define _ARCH_X86_64_TYPE_H_

#include <stdint.h>
#include <stddef.h>

#if __BITS_PER_LONG != 64
typedef int32_t ssize_t;
#else
typedef long ssize_t;
#endif

typedef int32_t pid_t;
typedef int64_t clock_t;
typedef uint64_t uid_t;

#endif