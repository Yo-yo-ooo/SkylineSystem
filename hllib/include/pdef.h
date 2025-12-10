#pragma once
#ifndef _ARCH_X86_64_PUBLIC_DEFINES_H
#define _ARCH_X86_64_PUBLIC_DEFINES_H

#include <stdint.h>
#include <stddef.h>

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#ifdef __GNUC__
#define PACK( __Declaration__ ) __Declaration__ __attribute__((packed))
#endif

#ifdef _MSC_VER
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop))
#endif

#define elif else if

#endif