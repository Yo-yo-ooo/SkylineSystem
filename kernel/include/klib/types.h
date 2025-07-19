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

typedef char symbol[];

typedef struct atomic_lock_t{
    uint32_t locked;
} atomic_lock_t;

#endif