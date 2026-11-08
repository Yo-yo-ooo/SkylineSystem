//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * lib/stdc/time.c — ISO C time/clock (SkylineSystem 用户态 libc)
 *
 *   time()   走用户态 syscall SYSCALL_TIME(17), 内核用 RTC 给出 UTC epoch 秒;
 *            失败 (返回负数) 时时返回 (time_t)-1 并置 errno。
 *   clock()  走用户态 syscall SYSCALL_MONOTONIC_MS(27) 的毫秒单调时钟,
 *            换算成 CLOCKS_PER_SEC=1e6 的 tick; 失败返回 (clock_t)-1。
 *
 * 宿主测试钩子 (tests/stdc):
 *   -DSKY_TIME_SOURCE=host_rtc_seconds
 *   -DSKY_CLOCK_SOURCE=host_monotonic_ms
 * 把内核调用替换成桩函数 —— 宿主进程里 syscall 17 是别的系统调用, 不能真发。
 */

#include <time.h>
#include <errno.h>
#include <base/arch/x86_64/syscalln.h>
#include <base/arch/x86_64/syscall.h>

#ifndef SKY_TIME_SOURCE
#define SKY_TIME_SOURCE() ((long)syscall(SYSCALL_TIME, 0, 0, 0, 0, 0, 0))
#else
/* 宿主测试钩子: 由测试提供 long host_* (void) 桩函数 */
extern long SKY_TIME_SOURCE(void);
#endif

#ifndef SKY_CLOCK_SOURCE
#define SKY_CLOCK_SOURCE() ((long)syscall(SYSCALL_MONOTONIC_MS, 0, 0, 0, 0, 0, 0))
#else
extern long SKY_CLOCK_SOURCE(void);
#endif

time_t time(time_t *tloc) {
    long r = SKY_TIME_SOURCE();

    if (r < 0) {
        errno = EOVERFLOW;
        return (time_t)-1;
    }
    if (tloc) *tloc = (time_t)r;
    return (time_t)r;
}

double difftime(time_t time1, time_t time0) {
    return (double)time1 - (double)time0;
}

clock_t clock(void) {
    long ms = SKY_CLOCK_SOURCE();

    if (ms < 0) return (clock_t)-1;
    return (clock_t)(ms * (CLOCKS_PER_SEC / 1000L));
}
