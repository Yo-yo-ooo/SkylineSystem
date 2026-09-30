#pragma once

#ifndef _KLIB_TYPES_H_
#define _KLIB_TYPES_H_
// tests/common/klib/types.h — shim: 去掉与 libc 冲突的 POSIX 类型重定义
// (ssize_t/pid_t/clock_t/uid_t/clockid_t), 保留组件需要的部分
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

typedef int8_t symbol[];

typedef int32_t spinlock_t;

/* loff_t 与 glibc 冲突, 宿主测试组件不使用, 已移除 */

typedef __signed__ __int128 __s128 __attribute__((aligned(16)));
typedef unsigned __int128 __u128 __attribute__((aligned(16)));

#define _unused

#endif
