//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: MIT
#pragma once
#ifndef _X86_64_SYSCALLN_H_
#define _X86_64_SYSCALLN_H_

#define SYSCALL_FOPEN           0
#define SYSCALL_FWRITE          1
#define SYSCALL_FREAD           2
#define SYSCALL_FCLOSE          3
#define SYSCALL_FLSEEK          4
#define SYSCALL_FSIZE           5

#define SYSCALL_THREAD_LAUNCH   6
#define SYSCALL_GETTID          7
#define SYSCALL_GETPID          8
#define SYSCALL_EXIT            9
#define SYSCALL_PMMAP           10
#define SYSCALL_YIELD           11
#define SYSCALL_LOAD            12
#define SYSCALL_LAUNCH          13

#define SYSCALL_MMAP            14
#define SYSCALL_MUNMAP          15

#define SYSCALL_SYSINFO         16
#define SYSCALL_TIME            17   /* RTC wall-clock seconds (sys_time) */
#define SYSCALL_KILL           19   /* terminate a process (sys_kill) */


#define SYSCALL_DEV_MMAP        21
/* 22: 创建目录 (仅 ext4 挂载)：返回 0 / 负 errno；保存原 reserved 槽。 */
#define SYSCALL_MKDIR           22
/* 23: 目录枚举 (仅 ext4)：(path, buf, cap, flags) -> 写入字节数 / -errno。
 * 输出为 "d|<name>\n" / "f|<name>\n" / "?|<name>\n" 文本行 + 结尾 NUL，
 * '.'/'..' 由内核过滤；缓冲不足时截断到完整行。Explorer 使用。 */
#define SYSCALL_READDIR         23
/* 24: 用户态串口调试输出 (与内核 syscalln.h 对齐) */
#define SYSCALL_DBGSOUT         24
#define SYSCALL_DEV_IOCTL       26
/* 27: 毫秒级单调时钟 (从启动起算, 不受 RTC 校准误差影响) */
#define SYSCALL_MONOTONIC_MS    27
/* 28: 毫秒级睡眠 (内核 Schedule::Sleep, 定时器轮真睡眠) */
#define SYSCALL_SLEEP_MS        28
/* 31: 子进程退出事件：buf = 用户 {int32 pid; int32 code;}
 *     返回 1=复制一条、0=无、负=-errno。替代 desktop 500ms kill 轮询。 */
#define SYSCALL_CHILD_EVENT     31

#endif