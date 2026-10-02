//SPDX-FileCopyrightText: 2026 Yo-yo-ooo
//SPDX-License-Identifier: GPL-2.0-only
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

/* P5-97: 补全后段编号的命名宏 (原 syscall.cpp 用裸数字 17-26) */
#define SYSCALL_TIME            17
#define SYSCALL_ARCH_PRCTL      18
#define SYSCALL_KILL            19
#define SYSCALL_GETRANDOM       20
#define SYSCALL_DEV_MMAP        21
#define SYSCALL_DBGSOUT         24
#define SYSCALL_DEV_GETINFO     25
#define SYSCALL_DEV_IOCTL       26

#endif