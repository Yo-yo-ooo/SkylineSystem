//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * time.h — ISO C 时间头 (SkylineSystem 用户态 libc)
 *
 * 只提供内嵌 Lua 5.4 需要的最小集合:
 *   time()      用户态 syscall SYSCALL_TIME(17) 读 RTC, 返回 UTC epoch 秒
 *   difftime()  ISO C 差值 (double)
 *   clock()     用户态 syscall SYSCALL_MONOTONIC_MS(27) 毫秒单调时钟
 *
 * 不提供 struct tm / localtime / strftime 等 (Lua 的 os.date/os.time
 * 不参与内嵌构建; 需要时再补)。
 */

#pragma once
#ifndef SKYLINE_LIB_TIME_H
#define SKYLINE_LIB_TIME_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* x86_64 SysV: long 为 64 位, 与内核 sys_time 的 time_t 一致 */
typedef long int time_t;
typedef long int clock_t;

/* POSIX/glibc 同样取 1000000: clock() 返回微秒级 tick */
#define CLOCKS_PER_SEC 1000000L

/* 返回 UTC epoch 秒; 失败返回 (time_t)-1 并置 errno。
   tloc 非 NULL 时同时写入。 */
time_t time(time_t *tloc);

/* 返回 time1 - time0, 单位秒。 */
double difftime(time_t time1, time_t time0);

/* 返回自进程/系统启动以来的处理器时间近似值 (毫秒单调时钟 × 1000)。
   失败返回 (clock_t)-1。 */
clock_t clock(void);

#ifdef __cplusplus
}
#endif

#endif /* SKYLINE_LIB_TIME_H */
