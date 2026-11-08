//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT

/*
 * signal.h — ISO C 信号头 (SkylineSystem 用户态 libc)
 *
 * 现状: 用户态没有异步信号投递机制, signal()/raise() 不实现。
 * 本头文件提供 ISO C 要求的 sig_atomic_t 与标准信号编号/处理器常量,
 * 足够让需要 <signal.h> 的第三方代码 (例如 Lua 5.4 的 lstate.h,
 * 它只需要 sig_atomic_t) 正常编译。
 */

#pragma once
#ifndef SKYLINE_LIB_SIGNAL_H
#define SKYLINE_LIB_SIGNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/* ISO C: 可以在信号处理器中原子访问的整型 */
typedef int sig_atomic_t;

/* ISO C 标准信号编号 (x86_64 Linux 取值) */
#define SIGABRT   6
#define SIGFPE    8
#define SIGILL    4
#define SIGINT    2
#define SIGSEGV  11
#define SIGTERM  15

/* ISO C 处理器常量 (标准要求可赋值给 signal 返回值; 这里只需可比较) */
#define SIG_DFL ((void (*)(int)) 0)
#define SIG_IGN ((void (*)(int)) 1)
#define SIG_ERR ((void (*)(int)) -1)

#ifdef __cplusplus
}
#endif

#endif /* SKYLINE_LIB_SIGNAL_H */
