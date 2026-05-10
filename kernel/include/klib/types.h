#pragma once

#ifndef _KLIB_TYPES_H_
#define _KLIB_TYPES_H_

#include <stdint.h>
#include <stddef.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;

typedef int64_t i64;
typedef int32_t i32;
typedef int16_t i16;
typedef int8_t i8;

typedef u64 usize;
typedef i64 isize;

typedef uintptr_t uptr;
typedef intptr_t iptr;

#define atomic_uint8_t  _Atomic  uint8_t
#define atomic_uint16_t _Atomic uint16_t
#define atomic_uint32_t _Atomic uint32_t
#define atomic_uint64_t _Atomic uint64_t

#define atomic_int8_t  _Atomic  int8_t
#define atomic_int16_t _Atomic int16_t
#define atomic_int32_t _Atomic int32_t
#define atomic_int64_t _Atomic int64_t

typedef int8_t symbol[];

typedef int32_t spinlock_t;

typedef uint64_t loff_t;

#if __BITS_PER_LONG != 64
typedef int32_t ssize_t;
#else
typedef long ssize_t;
#endif

typedef int32_t pid_t;
typedef int64_t clock_t;
typedef uint64_t uid_t;
typedef int32_t clockid_t;

typedef __signed__ __int128 __s128 __attribute__((aligned(16)));
typedef unsigned __int128 __u128 __attribute__((aligned(16)));


#define _unused

#endif